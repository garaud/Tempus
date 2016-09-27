#include <unordered_set>
#include <set>

#include "osm2tempus.h"
#include "writer.h"
#include "section_splitter.h"

struct Way
{
    std::vector<uint64_t> nodes;
    osm_pbf::Tags tags;
    bool ignored = false;
};


using WayCache = std::unordered_map<uint64_t, Way>;

struct TurnRestriction
{
    enum RestrictionType
    {
        NoLeftTurn,
        NoRightTurn,
        NoStraightOn,
        NoUTurn,
        OnlyRightTurn,
        OnlyLeftTurn,
        OnlyStraightOn,
        NoEntry,
        NoExit
    };
    RestrictionType type;
    uint64_t from_way;
    uint64_t via_node;
    uint64_t to_way;
};

///
/// Compute the (signed) angle (in degrees) between three points
float angle_3_points( float ax, float ay, float bx, float by, float cx, float cy )
{
    float abx = bx - ax; float aby = by - ay;
    float cbx = bx - cx; float cby = by - cy;

    float dot = (abx * cbx + aby * cby); // dot product
    float cross = (abx * cby - aby * cbx); // cross product

    return atan2(cross, dot) / M_PI * 180.;
}

struct RelationReader
{
    RelationReader() {}
    
    void node_callback( uint64_t /*osmid*/, double /*lon*/, double /*lat*/, const osm_pbf::Tags &/*tags*/ )
    {
    }

    void way_callback( uint64_t /*osmid*/, const osm_pbf::Tags& /*tags*/, const std::vector<uint64_t>& /*nodes*/ )
    {
    }
    
    void relation_callback( uint64_t /*osmid*/, const osm_pbf::Tags & tags, const osm_pbf::References & refs )
    {
        auto r_it = tags.find( "restriction" );
        auto t_it = tags.find( "type" );
        if ( r_it != tags.end() &&
             t_it != tags.end() && t_it->second == "restriction" ) {
            uint64_t from = 0, via_n = 0, to = 0;
            for ( const osm_pbf::Reference& r : refs ) {
                if ( r.role == "from" )
                    from = r.member_id;
                else if ( r.role == "via" ) {
                    via_n = r.member_id;
                }
                else if ( r.role == "to" )
                    to = r.member_id;
            }

            if ( from && via_n && to ) {
                auto type_it = restriction_types.find( r_it->second );
                if ( type_it != restriction_types.end() ) {
                    restrictions_.push_back( TurnRestriction({type_it->second, from, via_n, to}) );
                    via_nodes_ways_.emplace( via_n, std::vector<uint64_t>() );
                }
            }
        }
    }

    bool has_via_node( uint64_t node ) const { return via_nodes_ways_.find( node ) != via_nodes_ways_.end(); }

    void add_node_edge( uint64_t node, uint64_t way )
    {
        via_nodes_ways_[node].push_back( way );
    }

    void add_way_section( uint64_t way_id, uint64_t section_id, uint64_t node1, uint64_t node2 )
    {
        way_sections_[way_id].emplace( section_id, node1, node2 );
    }

    template <typename Progressor>
    void write_restrictions( const PointCache& points, Writer& writer, Progressor& progressor )
    {
        progressor( 0, restrictions_.size() );
        writer.begin_restrictions();
        size_t ri = 0;
        for ( const auto& tr : restrictions_ ) {
            progressor( ++ri, restrictions_.size() );
            // only process way - node - way relations
            if ( points.find( tr.from_way ) ||
                 !points.find( tr.via_node ) ||
                 points.find( tr.to_way ) )
                continue;
            // get the first edge until on the "from" way
            Section section_from;
            Section section_to;
            for ( auto section: way_sections_[tr.from_way] ) {
                if ( section.node2 == tr.via_node ) {
                    section_from = section;
                    break;
                }
                if ( section.node1 == tr.via_node ) {
                    section_from.id = section.id;
                    section_from.node1 = section.node2;
                    section_from.node2 = section.node1;
                    break;
                }
            }
            if ( section_from.id == 0 )
                continue;

            std::vector<Section> sections_to;
            for ( auto section: way_sections_[tr.to_way] ) {
                if ( section.node1 == tr.via_node || section.node2 == tr.via_node )
                    sections_to.push_back( section );
            }
            if ( sections_to.size() == 1 ) {
                section_to = sections_to[0];
                //std::cout << "1 " << section_from.id << " via " << tr.via_node << " to " << section_to.id << std::endl;
                writer.write_restriction( ++restriction_id, { section_from.id, section_to.id } );
            }
            else if ( sections_to.size() == 2 ) {
                // choose left or right
                float angle[2];
                int i = 0;
                for ( const auto& s : sections_to ) {
                    uint64_t n1 = s.node1;
                    uint64_t n2 = s.node2;
                    if ( tr.via_node == s.node2 )
                        std::swap( n1, n2 );
                    const auto& p1 = points.at( section_from.node1 );
                    const auto& p2 = points.at( tr.via_node );
                    const auto& p3 = points.at( n2 );
                    // compute the angle between the 3 points
                    // we could have used the orientation (determinant), but angle computation is more stable
                    angle[i] = angle_3_points( p1.lon(), p1.lat(), p2.lon(), p2.lat(), p3.lon(), p3.lat() );
                    i++;
                }
                if ( tr.type == TurnRestriction::NoLeftTurn || tr.type == TurnRestriction::OnlyLeftTurn ) {
                    // take the angle < 0
                    if ( angle[0] < 0 )
                        section_to = sections_to[0];
                    else
                        section_to = sections_to[1];
                }
                else if ( tr.type == TurnRestriction::NoRightTurn || tr.type == TurnRestriction::OnlyRightTurn ) {
                    // take the angle > 0
                    if ( angle[0] > 0 )
                        section_to = sections_to[0];
                    else
                        section_to = sections_to[1];
                }
                else if ( tr.type == TurnRestriction::NoStraightOn || tr.type == TurnRestriction::OnlyStraightOn ) {
                    // take the angle closer to 0
                    if ( abs(angle[0]) < abs(angle[1]) )
                        section_to = sections_to[0];
                    else
                        section_to = sections_to[1];
                }
                else {
                    std::cerr << "Ignoring restriction from " << tr.from_way << " to " << tr.to_way << " with type " << tr.type << std::endl;
                    continue;
                }

                // now distinguish between NoXXX and OnlyXXX restriction types
                if ( tr.type == TurnRestriction::NoLeftTurn ||
                     tr.type == TurnRestriction::NoRightTurn ||
                     tr.type == TurnRestriction::NoStraightOn ) {
                    // emit the restriction
                    //std::cout << "2 " << section_from.id << " via " << tr.via_node << " to " << section_to.id << std::endl;
                    writer.write_restriction( ++restriction_id, { section_from.id, section_to.id } );
                }
                else {
                    // OnlyXXX is like several NoXXX on the other edges
                    // we have to emit a restriction for every connected edges, except this one
                    for ( auto way : via_nodes_ways_.at( tr.via_node ) ) {
                        for ( auto section: way_sections_.at( way ) ) {
                            if ( section.node1 == tr.via_node || section.node2 == tr.via_node ) {
                                if ( section.id != section_to.id && section.id != section_from.id )
                                    //std::cout << "3 " << section_from.id << " via " << tr.via_node << " to " << section.id << std::endl;
                                    writer.write_restriction( ++restriction_id, { section_from.id, section.id } );
                            }
                        }
                    }
                }
                    
            }
        }
        writer.end_restrictions();
    }
    
private:
    const std::map<std::string, TurnRestriction::RestrictionType> restriction_types = {
        {"no_left_turn", TurnRestriction::NoLeftTurn},
        {"no_right_turn", TurnRestriction::NoRightTurn},
        {"no_straight_on", TurnRestriction::NoStraightOn},
        {"only_left_turn", TurnRestriction::OnlyLeftTurn},
        {"only_right_turn", TurnRestriction::OnlyRightTurn},
        {"only_straight_on", TurnRestriction::OnlyStraightOn},
        {"no_entry", TurnRestriction::NoEntry},
        {"no_exit", TurnRestriction::NoExit} };

    // maps a via node id to a list of ways that pass through it
    std::unordered_map<uint64_t, std::vector<uint64_t>> via_nodes_ways_;
    std::vector<TurnRestriction> restrictions_;
    struct Section
    {
        Section() : node1(0), node2(0), id(0) {}
        Section( uint64_t sid, uint64_t n1, uint64_t n2 ) : node1(n1), node2(n2), id(sid) {}
        uint64_t node1, node2;
        uint64_t id;
        bool operator<( const Section& other ) const
        {
            return id < other.id;
        }
    };
    std::map<uint64_t, std::set<Section>> way_sections_;

    uint64_t restriction_id = 0;
};

template <bool do_import_restrictions_ = false>
struct PbfReader
{
    PbfReader( RelationReader& restrictions ) :
        restrictions_( restrictions ),
        section_splitter_( points_ )
    {}
    
    void node_callback( uint64_t osmid, double lon, double lat, const osm_pbf::Tags &/*tags*/ )
    {
        points_.insert( osmid, PointCache::PointType(lon, lat) );
    }

    void way_callback( uint64_t osmid, const osm_pbf::Tags& tags, const std::vector<uint64_t>& nodes )
    {
        // ignore ways that are not highway
        if ( tags.find( "highway" ) == tags.end() )
            return;

        auto r = ways_.emplace( osmid, Way() );
        Way& w = r.first->second;
        w.tags = tags;
        w.nodes = nodes;
    }

    template <typename Progressor>
    void mark_points_and_ways( Progressor& progressor )
    {
        progressor( 0, ways_.size() );
        size_t i = 0;
        for ( auto way_it = ways_.begin(); way_it != ways_.end(); way_it++ ) {
            // mark each nodes as being used
            for ( uint64_t node: way_it->second.nodes ) {
                auto pt = points_.find( node );
                if ( pt ) {
                    if ( pt->uses() < 2 )
                        pt->set_uses( pt->uses() + 1 );
                }
                else {
                    // unknown point
                    way_it->second.ignored = true;
                }
                if ( do_import_restrictions_ ) { // static_if
                    // check if the node is involved in a restriction
                    if ( restrictions_.has_via_node( node ) ) {
                        restrictions_.add_node_edge( node, way_it->first );
                    }
                }
            }
            progressor( ++i, ways_.size() );
        }
    }

    ///
    /// Convert raw OSM ways to road sections. Sections are road parts between two intersections.
    template <typename Progressor>
    void write_sections( Writer& writer, Progressor& progressor )
    {
        progressor( 0, ways_.size() );
        size_t i = 0;
        writer.begin_sections();
        for ( auto way_it = ways_.begin(); way_it != ways_.end(); way_it++ ) {
            const Way& way = way_it->second;
            if ( way.ignored )
                continue;

            way_to_sections( way_it->first, way, writer );
            progressor( ++i, ways_.size() );
        }
        writer.end_sections();
    }

    template <typename Progressor>
    void write_nodes( Writer& writer, Progressor& progressor )
    {
        progressor( 0, points_.size() );
        size_t i = 0;
        writer.begin_nodes();
        for ( const auto& p : points_ ) {
            if ( p.second.uses() > 1 )
                writer.write_node( p.first, p.second.lat(), p.second.lon() );
            progressor( ++i, points_.size() );
        }
        writer.end_nodes();
    }

    void way_to_sections( uint64_t way_id, const Way& way, Writer& writer )
    {
        // split the way on intersections (i.e. node that are used more than once)
        bool section_start = true;
        uint64_t old_node = way.nodes[0];
        uint64_t node_from;
        std::vector<uint64_t> section_nodes;

        for ( size_t i = 1; i < way.nodes.size(); i++ ) {
            uint64_t node = way.nodes[i];
            const PointWithUses& pt = points_.at( node );
            if ( section_start ) {
                section_nodes.clear();
                section_nodes.push_back( old_node );
                node_from = old_node;
                section_start = false;
            }
            section_nodes.push_back( node );
            if ( i == way.nodes.size() - 1 || pt.uses() > 1 ) {
                section_splitter_( way_id, node_from, node, section_nodes, way.tags,
                                   [&](uint64_t lway_id, uint64_t lsection_id, uint64_t lnode_from, uint64_t lnode_to, const std::vector<Point>& lpts, const osm_pbf::Tags& ltags)
                                   {
                                       writer.write_section( lway_id, lsection_id, lnode_from, lnode_to, lpts, ltags );
                                       if ( do_import_restrictions_ ) { // static_if
                                           if ( restrictions_.has_via_node( lnode_from ) || restrictions_.has_via_node( lnode_to ) )
                                               restrictions_.add_way_section( lway_id, lsection_id, lnode_from, lnode_to );
                                       }
                                   });
                section_start = true;
            }
            old_node = node;
        }
    }

    void relation_callback( uint64_t /*osmid*/, const osm_pbf::Tags & /*tags*/, const osm_pbf::References & /*refs*/ )
    {
    }

    const PointCache& points() const { return points_; }

private:
    RelationReader& restrictions_;
    
    PointCache points_;
    WayCache ways_;

    SectionSplitter<PointCache> section_splitter_;
};


void single_pass_pbf_read( const std::string& filename, Writer& writer, bool do_write_nodes, bool do_import_restrictions )
{
    off_t ways_offset = 0, relations_offset = 0;
    osm_pbf::osm_pbf_offsets<StdOutProgressor>( filename, ways_offset, relations_offset );
    std::cout << "Ways offset: " << std::hex << ways_offset << std::endl;
    std::cout << "Relations offset: " << std::hex << relations_offset << std::endl;

    std::cout << "Relations ..." << std::endl;
    RelationReader r;
    osm_pbf::read_osm_pbf<RelationReader, StdOutProgressor>( filename, r, relations_offset );

    std::cout << "Nodes and ways ..." << std::endl;
    if ( do_import_restrictions ) {
        PbfReader<true> p( r );
        osm_pbf::read_osm_pbf<PbfReader<true>, StdOutProgressor>( filename, p, 0, relations_offset );
        std::cout << "Marking nodes and ways ..."  << std::endl;
        {
            StdOutProgressor prog;
            p.mark_points_and_ways( prog );
        }
        std::cout << "Writing sections ..."  << std::endl;
        {
            StdOutProgressor prog;
            p.write_sections( writer, prog );
        }

        if ( do_write_nodes ) {
            std::cout << "Writing nodes..." << std::endl;
            StdOutProgressor prog;
            p.write_nodes( writer, prog );
        }
        std::cout << "Writing restrictions ..." << std::endl;
        {
            StdOutProgressor prog;
            r.write_restrictions( p.points(), writer, prog );
        }
    }
    else {
        PbfReader<false> p( r );
        osm_pbf::read_osm_pbf<PbfReader<false>, StdOutProgressor>( filename, p, 0, relations_offset );
        std::cout << "Marking nodes and ways ..."  << std::endl;
        {
            StdOutProgressor prog;
            p.mark_points_and_ways( prog );
        }
        std::cout << "Writing sections ..."  << std::endl;
        {
            StdOutProgressor prog;
            p.write_sections( writer, prog );
        }

        if ( do_write_nodes ) {
            std::cout << "Writing nodes..." << std::endl;
            StdOutProgressor prog;
            p.write_nodes( writer, prog );
        }
    }
}
