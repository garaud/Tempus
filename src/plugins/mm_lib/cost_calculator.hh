/**
 *   Copyright (C) 2012-2013 IFSTTAR (http://www.ifsttar.fr)
 *   Copyright (C) 2012-2013 Oslandia <infos@oslandia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef AUTOMATION_LIB_COST_CALCULATOR_HH
#define AUTOMATION_LIB_COST_CALCULATOR_HH

#include "reverse_multimodal_graph.hh"
#include "speed_profile.hh"

namespace Tempus {

// Pedestrian speed in m/sec
#define DEFAULT_WALKING_SPEED 1.0
// Cycle speed in m/sec
#define DEFAULT_CYCLING_SPEED 5.0

// Time penalty to add when walking in and out from/to a public transport station
#define PT_STATION_PENALTY 0.1
#define POI_STATION_PENALTY 0.1

template <class RoadGraph>
double avg_road_travel_time( const RoadGraph& road_graph, const Road::Edge& road_e, double length, const TransportMode& mode,
                         double walking_speed = DEFAULT_WALKING_SPEED, double cycling_speed = DEFAULT_CYCLING_SPEED )
{
    if ( mode.speed_rule() == SpeedRuleCar ) {
        // take 60% of the speed limit
        return length / (road_graph[ road_e ].car_speed_limit() * 1000 * 0.60) * 60 ;
    }
    else if ( mode.speed_rule() == SpeedRulePedestrian ) { // Walking
        return length / (walking_speed * 1000) * 60 ;
    }
    else if ( mode.speed_rule() == SpeedRuleBicycle ) { // Bicycle
        return length / (cycling_speed * 1000) * 60 ;
    }
    else {
        return std::numeric_limits<double>::max() ;
    }
}


template <class RoadGraph>
double road_travel_time( const RoadGraph& road_graph, const Road::Edge& road_e, double length, double time, const TransportMode& mode,
                         double walking_speed = DEFAULT_WALKING_SPEED, double cycling_speed = DEFAULT_CYCLING_SPEED, const RoadEdgeSpeedProfile* profile = 0 )
{
    if ( (road_graph[ road_e ].traffic_rules() & mode.traffic_rules()) == 0 ) { // Not allowed mode 
        return std::numeric_limits<double>::infinity() ;
    }
    if (profile) {
        RoadEdgeSpeedProfile::PeriodIterator it, it_end;
        bool found = false;
        boost::tie(it, it_end) = profile->periods_after( road_graph[road_e].db_id(), mode.speed_rule(), time, found );
        if ( found ) {
            double t_end, t_begin, speed = 50000.0/60.0;
            t_begin = time;
            while ( (it != it_end) && (length > 0) ) {
                speed = it->second.speed * 1000.0 / 60.0; // km/h -> m/min
                t_end = it->first + it->second.length;
                length -= speed * (t_end - t_begin);
                t_begin = t_end;
                it++;
            }
            return t_begin + (length / speed) - time;
        }
    }
    return avg_road_travel_time( road_graph, road_e, length, mode, walking_speed, cycling_speed );
}

// Turning movement penalty function
template <typename AutomatonGraph>
double penalty ( const AutomatonGraph& graph, const typename boost::graph_traits< AutomatonGraph >::vertex_descriptor& v, unsigned traffic_rules )
{
    for (std::map<int, double >::const_iterator penaltyIt = graph[v].penalty_per_mode.begin() ; penaltyIt != graph[v].penalty_per_mode.end() ; penaltyIt++ )
    {
        // if the mode has a traffic rule in common with the penalty key
        if (traffic_rules & penaltyIt->first) return penaltyIt->second; 
    }
    return 0; 
}		

struct TimetableData {
    unsigned int trip_id;
    double arrival_time; 
};
	
struct FrequencyData {
    unsigned int trip_id; 
    double end_time; 
    double headway;
    double travel_time; 
};
	
// transport_mode -> Edge -> departure_time -> Timetable
typedef std::map<int, std::map<PublicTransport::Edge, std::map<double, TimetableData> > > TimetableMap; 
typedef std::map<int, std::map<PublicTransport::Edge, std::map<double, FrequencyData> > > FrequencyMap;

//
// Functor used by travel_time() for public transport edges, using internal timetables attached to the multimodal graph
template <class Graph>
struct pt2pt_time_internal_timetable_t
{
    pt2pt_time_internal_timetable_t( const Graph& graph, const Date& start_day, double min_transfer_time ) :
        graph_( graph ),
        start_day_( start_day ),
        min_transfer_time_( min_transfer_time )
    {}

    const Graph& graph_;
    const Date& start_day_;
    double min_transfer_time_;

    double operator()( const Multimodal::Edge& e,
                       const PublicTransport::Edge& pt_e,
                       db_id_t /*mode_id*/,
                       double initial_time,
                       double /*initial_shift_time*/,
                       double& /*final_shift_time*/,
                       db_id_t initial_trip_id,
                       db_id_t& final_trip_id,
                       double& wait_time ) const
    {
        if ( ! is_graph_reversed<Graph>::value ) {
            // Timetable travel time calculation
            const PublicTransport::Graph& pt_graph = *( e.source().pt_graph() );
            auto trip_time = next_departure( pt_graph, pt_e, start_day_, initial_time );
            if ( ! trip_time ) {
                return std::numeric_limits<double>::max();
            }
            // continue on the same trip
            if ( trip_time->trip_id() == initial_trip_id ) {
                final_trip_id = trip_time->trip_id(); 
                wait_time = 0;
                return trip_time->arrival_time() - initial_time ; 
            }
            // Else, no connection without transfer found, or first step
            // Look for a service after transfer_time
            auto trip_time2 = next_departure( pt_graph, pt_e, start_day_, initial_time + min_transfer_time_ );
            if ( trip_time2 ) {
                final_trip_id = trip_time2->trip_id();
                wait_time = trip_time2->departure_time() - initial_time;
                return trip_time2->arrival_time() - initial_time;
            }
        }
        // FIXME reversed
        // FIXME frequency-based
        return std::numeric_limits<double>::max();
    }
};

//
// Functor used by travel_time() for public transport edges, using external timetables
template <class Graph>
struct pt2pt_time_external_timetable_t
{
    pt2pt_time_external_timetable_t( const Graph& graph, const Date& start_day, double min_transfer_time,
                                     const TimetableMap& timetable, const TimetableMap& rtimetable, const FrequencyMap& frequency, const FrequencyMap& rfrequency ) :
        graph_( graph )
        , start_day_( start_day )
        , min_transfer_time_( min_transfer_time )
        , timetable_( timetable )
        , rtimetable_( rtimetable )
        , frequency_( frequency )
        , rfrequency_( rfrequency )
    {}

    const Graph& graph_;
    const Date& start_day_;
    double min_transfer_time_;
    const TimetableMap& timetable_; 
    const TimetableMap& rtimetable_; 
    const FrequencyMap& frequency_; 
    const FrequencyMap& rfrequency_; 

    double operator()( const Multimodal::Edge& /*e*/,
                       const PublicTransport::Edge& pt_e,
                       db_id_t mode_id,
                       double initial_time,
                       double initial_shift_time,
                       double& final_shift_time,
                       db_id_t initial_trip_id,
                       db_id_t& final_trip_id,
                       double& wait_time ) const
    {
        if ( ! is_graph_reversed<Graph>::value ) {
            // Timetable travel time calculation
            auto pt_e_it = timetable_.find( mode_id );
            if ( pt_e_it != timetable_.end() ) {
                // look for timetable of the given edge
                auto mit = pt_e_it->second.find( pt_e );
                if ( mit == pt_e_it->second.end() ) { // no timetable for this mode
                    return std::numeric_limits<double>::max(); 
                }

                // get the time, just after initial_time
                auto it = mit->second.lower_bound( initial_time ) ;
                if ( it == mit->second.end() ) { // no service after this time
                    return std::numeric_limits<double>::max(); 
                }

                // Continue on the same trip
                if ( it->second.trip_id == initial_trip_id ) { 
                    final_trip_id = it->second.trip_id; 
                    wait_time = 0;
                    return it->second.arrival_time - initial_time;
                } 
                // Else, no connection without transfer found, or first step
                // Look for a service after transfer_time
                it = mit->second.lower_bound( initial_time + min_transfer_time_ ); 
                if ( it != mit->second.end() ) {
                    final_trip_id = it->second.trip_id;
                    wait_time = it->first - initial_time;
                    return it->second.arrival_time - initial_time;
                }
            }
            else if (frequency_.find( mode_id ) != frequency_.end() ) {
                auto pt_re_it = frequency_.find( mode_id );
                auto mit = pt_re_it->second.find( pt_e );
                if ( mit == pt_re_it->second.end() ) { // no timetable for this mode and edge
                    return std::numeric_limits<double>::max(); 
                }
                auto it = mit->second.upper_bound( initial_time );
                if (it == mit->second.begin() ) { // nothing before this time
                    return std::numeric_limits<double>::max();
                }
                it--;
                if ( it->second.end_time < initial_time ) {
                    // frequency-based trips are supposed not to overlap
                    // so it means this trip is not in service anymore at initial_time
                    return std::numeric_limits<double>::max();                        
                }
                if ( it->second.trip_id == initial_trip_id ) {
                    final_trip_id = it->second.trip_id; 
                    wait_time = 0;
                    return it->second.travel_time;
                } 
                // Else, no connection without transfer found
                it = mit->second.upper_bound( initial_time + min_transfer_time_ ); 
                if ( it != mit->second.begin() ) { 
                    it--; 
                    if ( it->second.end_time >= initial_time + min_transfer_time_ ) {
                        final_trip_id = it->second.trip_id ; 
                        wait_time = it->second.headway/2 ; 
                        return it->second.travel_time + wait_time;
                    }
                }
            }
        }
        else {
            // reverse graph
            auto pt_e_it = rtimetable_.find( mode_id );
            if ( pt_e_it != rtimetable_.end() ) {
                // look for timetable of the given edge
                auto mit = pt_e_it->second.find( pt_e );
                if ( mit == pt_e_it->second.end() ) { // no timetable for this mode and edge
                    return std::numeric_limits<double>::max(); 
                }
                double rinitial_time = -initial_time - initial_shift_time;
                // get the time, just before initial_time (upper_bound - 1)
                auto it = mit->second.upper_bound( rinitial_time ) ;
                if ( it == mit->second.begin() ) { // nothing before this time
                    return std::numeric_limits<double>::max(); 
                }
                it--;

                final_trip_id = it->second.trip_id; 
                wait_time = rinitial_time - it->first;
                final_shift_time += wait_time;
                return it->first - it->second.arrival_time;
            }
            else if ( rfrequency_.find( mode_id ) != rfrequency_.end() ) {
                double rinitial_time = -initial_time - initial_shift_time;
                auto pt_re_it = rfrequency_.find( mode_id );
                auto mit = pt_re_it->second.find( pt_e );
                if ( mit == pt_re_it->second.end() ) { // no timetable for this mode
                    return std::numeric_limits<double>::max(); 
                }
                auto it = mit->second.upper_bound( rinitial_time );
                if (it == mit->second.end() ) { // nothing before this time
                    return std::numeric_limits<double>::max();
                }
                if ( it->second.end_time >= rinitial_time ) {
                    // frequency-based trips are supposed not to overlap
                    // so it means this trip is not in service anymore at initial_time
                    return std::numeric_limits<double>::max();                        
                }
                if ( it->second.trip_id == initial_trip_id ) {
                    final_trip_id = it->second.trip_id; 
                    wait_time = 0;
                    return it->second.travel_time;
                } 
                // Else, no connection without transfer found
                it = mit->second.upper_bound( rinitial_time - min_transfer_time_ ); 
                if ( it != mit->second.end() ) { 
                    if ( it->second.end_time < rinitial_time - min_transfer_time_ ) {
                        final_trip_id = it->second.trip_id;
                        wait_time = it->second.headway/2;
                        return it->second.travel_time + wait_time;
                    }
                }
            }
        }
        return std::numeric_limits<double>::max();
    }
};

template <typename Graph, typename pt2pt_foo_t>
class CostCalculatorT {
public: 
    // Constructor 
    CostCalculatorT( const Graph& graph, const Date& start_day,
                     const std::vector<db_id_t>& allowed_transport_modes,
                     double walking_speed, double cycling_speed, 
                     double min_transfer_time, double car_parking_search_time, boost::optional<Road::Vertex> private_parking,
                     const RoadEdgeSpeedProfile* profile,
                     pt2pt_foo_t pt2pt_foo ) :
        graph_( graph ),
        start_day_( start_day ),
        allowed_transport_modes_( allowed_transport_modes ),
        walking_speed_( walking_speed ), cycling_speed_( cycling_speed ), 
        min_transfer_time_( min_transfer_time ), car_parking_search_time_( car_parking_search_time ),
        private_parking_( private_parking ),
        speed_profile_( profile ),
        pt2pt_foo_( pt2pt_foo )
    {
    }
		
    // Multimodal travel time function
    double travel_time( const Multimodal::Edge& e, db_id_t mode_id, double initial_time, double initial_shift_time, double& final_shift_time, db_id_t initial_trip_id, db_id_t& final_trip_id, double& wait_time ) const
    {
        // default (for non-PT edges)
        final_trip_id = 0;

        final_shift_time = initial_shift_time;
        wait_time = 0.0;

        const TransportMode& mode = graph_.transport_modes().find( mode_id )->second;
        if ( std::find(allowed_transport_modes_.begin(), allowed_transport_modes_.end(), mode_id) != allowed_transport_modes_.end() ) 
        {
            switch ( e.connection_type() ) {  
            case Multimodal::Edge::Road2Road: {
                double c = road_travel_time( graph_.road(), e.road_edge(), graph_.road()[ e.road_edge() ].length(), initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ); 
                return c;
            }
                break;
		
            case Multimodal::Edge::Road2Transport: {
                double add_cost = 0.0;
                if ( is_graph_reversed<Graph>::value ) {
                    if ( initial_trip_id != 0 ) {
                        // we are "coming" from a Transport2Transport
                        wait_time = min_transfer_time_;
                        add_cost = min_transfer_time_;
                    }
                }

                // find the road section where the stop is attached to
                const PublicTransport::Graph& pt_graph = *( e.target().pt_graph() );
                double abscissa = pt_graph[ e.target().pt_vertex() ].abscissa_road_section();
                Road::Edge road_e = pt_graph[ e.target().pt_vertex() ].road_edge();
						
                // if we are coming from the start point of the road
                if ( source( road_e, graph_.road() ) == e.source().road_vertex() ) {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * abscissa, initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + PT_STATION_PENALTY + add_cost;
                }
                // otherwise, that is the opposite direction
                else {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * (1 - abscissa), initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + PT_STATION_PENALTY + add_cost;
                }
            }
                break; 
					
            case Multimodal::Edge::Transport2Road: {
                // find the road section where the stop is attached to
                const PublicTransport::Graph& pt_graph = *( e.source().pt_graph() );
                double abscissa = pt_graph[ e.source().pt_vertex() ].abscissa_road_section();
                Road::Edge road_e = pt_graph[ e.source().pt_vertex() ].road_edge();
						
                // if we are coming from the start point of the road
                if ( target( road_e, graph_.road() ) == e.target().road_vertex() ) {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * (1 - abscissa), initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + PT_STATION_PENALTY;
                }
                // otherwise, that is the opposite direction
                else {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * abscissa, initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + PT_STATION_PENALTY;
                }
            } 
                break;
					
            case Multimodal::Edge::Transport2Transport: { 
                PublicTransport::Edge pt_e;
                bool found = false;
                boost::tie( pt_e, found ) = public_transport_edge( e );
                BOOST_ASSERT(found);

                return pt2pt_foo_( e, pt_e, mode_id, initial_time, initial_shift_time, final_shift_time, initial_trip_id, final_trip_id, wait_time );
            }
                break; 
					
            case Multimodal::Edge::Road2Poi: {
                Road::Edge road_e = e.target().poi()->road_edge();
                double abscissa = e.target().poi()->abscissa_road_section();

                // if we are coming from the start point of the road
                if ( source( road_e, graph_.road() ) == e.source().road_vertex() ) {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * abscissa, initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + POI_STATION_PENALTY;
                }
                // otherwise, that is the opposite direction
                else {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * (1 - abscissa), initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + POI_STATION_PENALTY;
                }
            }
                break;
            case Multimodal::Edge::Poi2Road: {
                Road::Edge road_e = e.source().poi()->road_edge();
                double abscissa = e.source().poi()->abscissa_road_section();

                // if we are coming from the start point of the road
                if ( source( road_e, graph_.road() ) == e.source().road_vertex() ) {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * abscissa, initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + POI_STATION_PENALTY;
                }
                // otherwise, that is the opposite direction
                else {
                    return road_travel_time( graph_.road(), road_e, graph_.road()[ road_e ].length() * (1 - abscissa), initial_time, mode,
                                             walking_speed_, cycling_speed_, speed_profile_ ) + POI_STATION_PENALTY;
                }
            }
                break;
            default: {
						
            }
            }
        }
        return std::numeric_limits<double>::max(); 
    }
		
    // Mode transfer time function : returns numeric_limits<double>::max() when the mode transfer is impossible
    double transfer_time( const Multimodal::Edge& edge, const TransportMode& initial_mode, const TransportMode& final_mode ) const
    {
        double transf_t = 0;
        if (initial_mode.db_id() == final_mode.db_id() ) {
            return 0.0;
        }

        const Multimodal::Vertex& src = edge.source();
        const Multimodal::Vertex& tgt = edge.target();

        if ( initial_mode.is_public_transport() && final_mode.is_public_transport() ) {
            return 0.0;
        }

        // park shared vehicle
        if ( ( transf_t < std::numeric_limits<double>::max() ) && initial_mode.must_be_returned() ) {
            if (( tgt.type() == Multimodal::Vertex::Poi ) && ( tgt.poi()->has_parking_transport_mode( initial_mode.db_id() ) )) {
                // FIXME replace 1 by time to park a shared vehicle
                transf_t += 1;
            }
            else {
                transf_t = std::numeric_limits<double>::max();
            }                
        }
        // Parking search time for initial mode
        else if ( ( transf_t < std::numeric_limits<double>::max() ) && initial_mode.need_parking() ) {
            if ( (tgt.type() == Multimodal::Vertex::Poi ) && ( tgt.poi()->has_parking_transport_mode( initial_mode.db_id() ) ) ) {
                // FIXME more complex than that
                if (initial_mode.traffic_rules() & TrafficRuleCar ) 
                    transf_t += car_parking_search_time_ ; // Personal car
                // For bicycle, parking search time = 0
            }
            // park on the private parking
            else if ( private_parking_ && !initial_mode.is_shared() && tgt.road_vertex() == private_parking_.get() ) {
                transf_t += 1;
            }
            // park on streets
            else if ( ( tgt.type() == Multimodal::Vertex::Road ) && (src.type() == Multimodal::Vertex::Road) &&
                      ( (graph_.road()[ edge.road_edge() ].parking_traffic_rules() & initial_mode.traffic_rules()) > 0 ) ) {
                if (initial_mode.traffic_rules() & TrafficRuleCar ) 
                    transf_t += car_parking_search_time_ ; // Personal car
                // For bicycle, parking search time = 0
            }
            else {
                transf_t = std::numeric_limits<double>::max();
            }
        }


        // take a shared vehicle from a POI
        if ( ( transf_t < std::numeric_limits<double>::max() ) && final_mode.is_shared() ) {
            if (( src.type() == Multimodal::Vertex::Poi ) && ( src.poi()->has_parking_transport_mode( final_mode.db_id() ) )) {
                // FIXME replace 1 by time to take a shared vehicle
                transf_t += 1;
            }
            else {
                transf_t = std::numeric_limits<double>::max();
            }
        }
        // Taking vehicle time for final mode 
        else if ( ( transf_t < std::numeric_limits<double>::max() ) && ( final_mode.need_parking() ) ) {
            if (( src.type() == Multimodal::Vertex::Poi ) && final_mode.is_shared() && ( src.poi()->has_parking_transport_mode( final_mode.db_id() ) )) {
                // shared vehicles parked on a POI
                transf_t += 1;
            }
            else if ( private_parking_ && !final_mode.is_shared() && src.road_vertex() == private_parking_.get() ) {
                // vehicles parked on the private parking
                transf_t += 1;
            }
            else {
                transf_t = std::numeric_limits<double>::max();
            }
        }

        return transf_t; 
    }
		
protected:
    const Graph& graph_;
    const Date start_day_;
    const std::vector<db_id_t> allowed_transport_modes_;
    const double walking_speed_; 
    const double cycling_speed_; 
    const double min_transfer_time_; 
    const double car_parking_search_time_; 
    boost::optional<Road::Vertex> private_parking_;
    const RoadEdgeSpeedProfile* speed_profile_;
    pt2pt_foo_t pt2pt_foo_;
};

//
// Cost calculator that uses internal graph timetables
template <typename Graph>
class CostCalculatorInternalTimetable : public CostCalculatorT<Graph, pt2pt_time_internal_timetable_t<Graph>>
{
 public:
    CostCalculatorInternalTimetable( const Graph& graph, const Date& start_day,
                                     const std::vector<db_id_t>& allowed_transport_modes,
                                     double walking_speed, double cycling_speed, 
                                     double min_transfer_time, double car_parking_search_time, boost::optional<Road::Vertex> private_parking,
                                     const RoadEdgeSpeedProfile* profile = 0 ):
        CostCalculatorT<Graph,
                        pt2pt_time_internal_timetable_t<Graph>>( graph, start_day, allowed_transport_modes,
                                                                 walking_speed, cycling_speed,
                                                                 min_transfer_time, car_parking_search_time, private_parking,
                                                                 profile,
                                                                 pt2pt_time_internal_timetable_t<Graph>( graph, start_day, min_transfer_time ) )
    {}
};

//
// Cost calculator that uses external graph timetables
// FIXME to deprecate (only use internal timetables in the future)
template <typename Graph>
class CostCalculatorExternalTimetable : public CostCalculatorT<Graph, pt2pt_time_external_timetable_t<Graph>>
{
 public:
    CostCalculatorExternalTimetable( const Graph& graph, const Date& start_day,
                                     const TimetableMap& timetable, const TimetableMap& rtimetable, const FrequencyMap& frequency, const FrequencyMap& rfrequency,
                                     const std::vector<db_id_t>& allowed_transport_modes,
                                     double walking_speed, double cycling_speed, 
                                     double min_transfer_time, double car_parking_search_time, boost::optional<Road::Vertex> private_parking,
                                     const RoadEdgeSpeedProfile* profile = 0 ):
        CostCalculatorT<Graph,
                        pt2pt_time_external_timetable_t<Graph>>( graph, start_day, allowed_transport_modes,
                                                                 walking_speed, cycling_speed,
                                                                 min_transfer_time, car_parking_search_time, private_parking,
                                                                 profile,
                                                                 pt2pt_time_external_timetable_t<Graph>( graph, start_day, min_transfer_time,
                                                                                                         timetable, rtimetable, frequency, rfrequency ) )
    {}
};

}

#endif
