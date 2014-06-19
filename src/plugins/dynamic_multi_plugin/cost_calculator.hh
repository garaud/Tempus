

using namespace std; 

namespace Tempus {

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
	
    typedef std::map<PublicTransport::Edge, std::map<double, TimetableData> > TimetableMap; 
    typedef std::map<PublicTransport::Edge, std::map<double, FrequencyData> > FrequencyMap; 
	
    class CostCalculator {
    public: 
        // Constructor 
        CostCalculator( TimetableMap& timetable, FrequencyMap& frequency, 
                        unsigned int allowed_transport_types, map<Multimodal::Vertex, unsigned int>& vehicle_nodes, 
                        double walking_speed, double cycling_speed, 
                        double min_transfer_time, double car_parking_search_time ) : 
            timetable_( timetable ), frequency_( frequency ), 
            allowed_transport_types_( allowed_transport_types ), vehicle_nodes_( vehicle_nodes ), 
            walking_speed_( walking_speed ), cycling_speed_( cycling_speed ), 
            min_transfer_time_( min_transfer_time ), car_parking_search_time_( car_parking_search_time )  { }; 
		
        // Multimodal travel time function
        double travel_time( Multimodal::Graph& graph, Multimodal::Edge& e, unsigned int mode, double initial_time, db_id_t initial_trip_id, db_id_t& final_trip_id, double& wait_time ) const
        {
            if ( (allowed_transport_types_ & mode) != 0 ) 
            {
                switch ( e.connection_type() ) {  
                case Multimodal::Edge::Road2Road: {
                    Road::Edge road_e;
                    bool found;
                    boost::tie( road_e, found ) = road_edge( e );
										
                    return road_travel_time( graph.road, road_e, graph.road[ road_e ].length, mode ); 
                }
                    break;
		
                case Multimodal::Edge::Road2Transport: {
                    // find the road section where the stop is attached to
                    const PublicTransport::Graph& pt_graph = *( e.target.pt_graph );
                    double abscissa = pt_graph[ e.target.pt_vertex ].abscissa_road_section; 
                    Road::Edge road_e = pt_graph[ e.target.pt_vertex ].road_edge; 
						
                    // if we are coming from the start point of the road
                    if ( source( road_e, graph.road ) == e.source.road_vertex ) 
                        return road_travel_time( graph.road, road_e, graph.road[ road_e ].length * abscissa, mode ) ;
                    // otherwise, that is the opposite direction
                    else return road_travel_time( graph.road, road_e, graph.road[ road_e ].length * (1 - abscissa), mode ) ; 
                }
                    break; 
					
                case Multimodal::Edge::Transport2Road: {
                    // find the road section where the stop is attached to
                    const PublicTransport::Graph& pt_graph = *( e.source.pt_graph );
                    double abscissa = pt_graph[ e.source.pt_vertex ].abscissa_road_section; 
                    Road::Edge road_e = pt_graph[ e.source.pt_vertex ].road_edge; 
						
                    // if we are coming from the start point of the road
                    if ( source( road_e, graph.road ) == e.source.road_vertex ) 
                        return road_travel_time( graph.road, road_e, graph.road[ road_e ].length * abscissa, mode ) ;
                    // otherwise, that is the opposite direction
                    else return road_travel_time( graph.road, road_e, graph.road[ road_e ].length * (1 - abscissa), mode ) ; 
                } 
                    break;
					
                case Multimodal::Edge::Transport2Transport: { 
                    PublicTransport::Edge pt_e;
                    bool found = false;
                    boost::tie( pt_e, found ) = public_transport_edge( e );
						
                    // Timetable travel time calculation
                    if (timetable_.find( pt_e ) != timetable_.end() ) {
                        std::map< double, TimetableData >::iterator it = timetable_.at( pt_e ).lower_bound( initial_time ) ;  
                        if ( it != timetable_.at( pt_e ).end() ) { 								
                            if (it->second.trip_id == initial_trip_id ) { 
                                final_trip_id = it->second.trip_id; 
                                wait_time = 0; 
                                return it->second.arrival_time - initial_time ; 
                            } 
                            else { // No connection without transfer found
                                it = timetable_.at( pt_e ).lower_bound( initial_time + min_transfer_time_ ); 
                                if ( it != timetable_.at( pt_e ).end() ) {
                                    final_trip_id = it->second.trip_id; 
                                    wait_time = it->first - initial_time ; 
                                    return it->second.arrival_time - initial_time ; 
                                }
                            }
                        }
                    } 
                    else if (frequency_.find( pt_e ) != frequency_.end() ) {
                        std::map<double, FrequencyData>::iterator it = frequency_.at( pt_e ).lower_bound( initial_time );
                        if (it != frequency_.at( pt_e ).begin() ) {
                            it--; 
                            if (it->second.trip_id == initial_trip_id  && ( it->second.end_time >= initial_time ) ) { // Connection without transfer
                                final_trip_id = it->second.trip_id; 
                                wait_time = 0; 
                                return it->second.travel_time ; 
                            } 
                            else { // No connection without transfer found
                                it = frequency_.at( pt_e ).upper_bound( initial_time + min_transfer_time_ ); 
                                if ( it != frequency_.at( pt_e ).begin() ) { 
                                    it--; 
                                    if ( it->second.end_time >= initial_time + min_transfer_time_ ) {
                                        final_trip_id = it->second.trip_id ; 
                                        wait_time = it->second.headway/2 ; 
                                        return it->second.travel_time + wait_time ; 
                                    }
                                }  
                            }
                        }
                    }
                }
                    break; 
					
                default: {
						
                }
                }
            }
            return std::numeric_limits<double>::max(); 
        }
		
        // Road travel time function: called by the multimodal one - static version
        double road_travel_time( Road::Graph& road_graph, Road::Edge& road_e, double length, unsigned int mode ) const
        {
            if ( (road_graph[ road_e ].transport_type & mode) == 0 ) // Not allowed mode 
                return std::numeric_limits<double>::infinity() ;
            else if ( mode == 1 || mode == 256 ) // Car
                return length / (road_graph[ road_e ].car_average_speed * 1000) * 60 ; 
            else if ( mode == 2 ) // Walking
                return length / (walking_speed_ * 1000) * 60 ; 
            else if ( mode == 4 || mode == 128 ) // Bicycle
                return length / (cycling_speed_ * 1000) * 60 ; 
            else return std::numeric_limits<double>::max() ;
        }
		
        // Turning movement penalty function
        template <typename AutomatonGraph>
        double penalty ( const AutomatonGraph& graph, typename boost::graph_traits< AutomatonGraph >::vertex_descriptor& v, unsigned int mode ) const
        {
            for (std::map<int, double >::const_iterator penaltyIt = graph[v].penaltyPerMode.begin() ; penaltyIt != graph[v].penaltyPerMode.end() ; penaltyIt++ )
            {
                if (mode & penaltyIt->first) return penaltyIt->second; 
            }
            return 0; 
        }
		
        // Mode transfer time function : returns numeric_limits<double>::max() when the mode transfer is impossible
        double transfer_time( const Multimodal::Graph& graph, const Multimodal::Vertex& v, db_id_t initial_mode_id, db_id_t final_mode_id ) const
        {
            double transf_t = 0;
            if (initial_mode_id != final_mode_id ) {
                const TransportMode& initial_mode = graph.transport_modes().at( initial_mode_id );
                const TransportMode& final_mode = graph.transport_modes().at( final_mode_id );
                // Parking search time for initial mode
                if ( initial_mode.need_parking() ) {
                    if ( ( ( v.type == Multimodal::Vertex::Road )  &&
                           ( (graph.road[ v.road_vertex ].parking_traffic_rules() & initial_mode.traffic_rules() ) > 0 ) )
                         || ( (v.type == Multimodal::Vertex::Poi ) && ( (v.poi)->parking_transport_mode() == initial_mode_id ) ) )
                    {
                        // FIXME
                        //if (initial_mode == 1) 
                        //    transf_t += car_parking_search_time_ ; // Personal car
                        // For bicycle, parking search time = 0
                    }
                    else
                        transf_t = std::numeric_limits<double>::max(); 
                }

#if 0
                // Taking vehicle time for final mode 
                if ( ( transf_t < std::numeric_limits<double>::max() ) && ( final_mode.need_parking() ) ) {
                    if ( ( vehicle_nodes_.find( v ) != vehicle_nodes_.end() ) && ( (vehicle_nodes_[v] & final_mode) > 0 ) )
                        transf_t += 1; 
                    else transf_t = std::numeric_limits<double>::max(); 
                }
#endif
            }

            return transf_t; 
        }
		
    protected:
        TimetableMap& timetable_; 
        FrequencyMap& frequency_; 
        unsigned int allowed_transport_types_; 
        map< Multimodal::Vertex, unsigned int >& vehicle_nodes_; 
        double walking_speed_; 
        double cycling_speed_; 
        double min_transfer_time_; 
        double car_parking_search_time_; 
    }; 

}
	
	
