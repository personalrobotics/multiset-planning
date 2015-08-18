/* File: RoadmapGenHalton.h
 * Author: Chris Dellin <cdellin@gmail.com>
 * Copyright: 2015 Carnegie Mellon University
 * License: BSD
 */

namespace ompl_multiset
{

// for now this is an r-disk prm,
// uniform milestone sampling with given seed,
// uses the space's default sampler
//template <class Graph, class VertexIndexMap, class EdgeIndexMap//,
   //class StateMap, class SubgraphMap, class IsShadowMap, class DistanceMap
//   >
template <class RoadmapGenSpec>
class RoadmapGenHaltonDens : public RoadmapGenSpec
{
   typedef typename RoadmapGenSpec::BaseGraph Graph;
   typedef typename RoadmapGenSpec::BaseVState VState;
   typedef typename RoadmapGenSpec::BaseEDistance EDistance;
   typedef typename RoadmapGenSpec::BaseVSubgraph VSubgraph;
   typedef typename RoadmapGenSpec::BaseESubgraph ESubgraph;
   typedef typename RoadmapGenSpec::BaseVShadow VShadow;

   typedef boost::graph_traits<Graph> GraphTypes;
   typedef typename GraphTypes::vertex_descriptor Vertex;
   typedef typename GraphTypes::edge_descriptor Edge;
   typedef typename boost::property_traits<VState>::value_type::element_type StateCon;
   
public:
   RoadmapGenHaltonDens(
      const ompl::base::StateSpacePtr space,
      const std::string args):
      RoadmapGenSpec(space,"RoadmapGenHaltonDens",args,0),
      dim(0),
      bounds(0),
      num_subgraphs_generated(0),
      vertices_generated(0),
      edges_generated(0)
   {
      // check that we're in a real vector state space
      if (space->getType() != ompl::base::STATE_SPACE_REAL_VECTOR)
         throw std::runtime_error("RoadmapGenHaltonDens only supports rel vector state spaces!");
      dim = space->getDimension();
      if (0 == ompl_multiset::util::get_prime(dim-1))
         throw std::runtime_error("not enough primes hardcoded!");
      bounds = space->as<ompl::base::RealVectorStateSpace>()->getBounds();
      int ret = sscanf(args.c_str(), "n_perbatch=%u radius_firstbatch=%lf",
         &n_perbatch, &radius_firstbatch);
      if (ret != 2)
         throw std::runtime_error("bad args to RoadmapGenHaltonDens!");
      if (args != ompl_multiset::util::sf("n_perbatch=%u radius_firstbatch=%s",
         n_perbatch, ompl_multiset::util::double_to_text(radius_firstbatch).c_str()))
      {
         throw std::runtime_error("args not in canonical form!");
      }
   }
   ~RoadmapGenHaltonDens() {}
   
   std::size_t get_num_subgraphs_generated()
   {
      return num_subgraphs_generated;
   }
   
   void generate(
      Graph & g,
      std::size_t num_subgraphs_desired,
      VState state_map,
      EDistance distance_map,
      VSubgraph vertex_subgraph_map,
      ESubgraph edge_subgraph_map,
      VShadow is_shadow_map)
   {
      while (num_subgraphs_generated < num_subgraphs_desired)
      {
         // compute radius,
         double radius = radius_firstbatch
            * pow(1./(num_subgraphs_generated+1.), 1./dim);
         printf("NEW RADIUS: %f\n", radius);
         
         while (num_vertices(g) < (num_subgraphs_generated+1) * n_perbatch)
         {
            Vertex v_new = add_vertex(g);
            
            put(vertex_subgraph_map, v_new, num_subgraphs_generated);
            put(is_shadow_map, v_new, false);
            
            // allocate a new state for this vertex
            get(state_map, v_new).reset(new StateCon(this->space.get()));
            ompl::base::State * v_state = get(state_map, v_new)->state;
            double * values = v_state->as<ompl::base::RealVectorStateSpace::StateType>()->values;
            for (unsigned int ui=0; ui<dim; ui++)
               values[ui] = bounds.low[ui] + (bounds.high[ui] - bounds.low[ui])
                  * ompl_multiset::util::halton(
                     ompl_multiset::util::get_prime(ui), vertices_generated);
                     
            // allocate new undirected edges
            for (unsigned int ui=0; ui<num_vertices(g)-1; ui++)
            {
               Vertex v_other = vertex(ui, g);
               double dist = this->space->distance(
                  get(state_map, v_new)->state,
                  get(state_map, v_other)->state);
               if (radius < dist)
                  continue;
               Edge e = add_edge(v_new, v_other, g).first;
               put(distance_map, e, dist);
               put(edge_subgraph_map, e, num_subgraphs_generated);
               edges_generated++;
            }
            
            vertices_generated++;
         }
         num_subgraphs_generated++;
      }
   }
   
   void serialize()
   {
   }
   
   void deserialize()
   {
   }
   
private:
   // from space
   unsigned int dim;
   ompl::base::RealVectorBounds bounds;
   // from id
   unsigned int n_perbatch; // per batch
   double radius_firstbatch; // of first batch
   // progress
   std::size_t num_subgraphs_generated;
   std::size_t vertices_generated;
   std::size_t edges_generated;
};

} // namespace ompl_multiset
