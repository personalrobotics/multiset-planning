/* File: lazy_prm.cpp
 * Author: Chris Dellin <cdellin@gmail.com>
 * Copyright: 2015 Carnegie Mellon University
 * License: BSD
 */

#include <fstream>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>

#include <ompl/base/Planner.h>
#include <ompl/base/StateSpace.h>
#include <ompl/base/goals/GoalState.h>
#include <ompl/geometric/PathGeometric.h>

// TEMP for stringify
#include <ompl/base/spaces/RealVectorStateSpace.h>

#include <pr_bgl/graph_io.h>
#include <pr_bgl/string_map.h>
#include <pr_bgl/edge_indexed_graph.h>
#include <pr_bgl/overlay_manager.h>
#include <pr_bgl/lazysp.h>
#include <pr_bgl/heap_indexed.h>

#include <ompl_multiset/BisectPerm.h>
#include <ompl_multiset/RoadmapGen.h>
#include <ompl_multiset/family.h>
#include <ompl_multiset/family_effort_model.h>
#include <ompl_multiset/family_planner.h>
#include <ompl_multiset/lazysp_log_visitor.h>


namespace ompl_multiset
{

inline void stringify_to_x(const std::string & in, ompl_multiset::StateConPtr & repr)
{
   repr.reset();
   //repr = atof(in.c_str());
}
inline void stringify_from_x(std::string & repr, const ompl_multiset::StateConPtr & in)
{
   unsigned int dim = in->space->getDimension();
   ompl::base::RealVectorStateSpace::StateType * state
      = in->state->as<ompl::base::RealVectorStateSpace::StateType>();
   repr.clear();
   for (unsigned int ui=0; ui<dim; ui++)
   {
      if (ui)
         repr += " ";
      std::string component_repr;
      pr_bgl::stringify_from_x(component_repr, (double)state->values[ui]);
      repr += component_repr;
   }
}

} // namespace multiset


namespace {

ompl::base::SpaceInformationPtr
get_bogus_si(const ompl_multiset::Family & family)
{
   if (!family.subsets.size())
      throw std::runtime_error("family must be non-empty!");
   const ompl::base::StateSpacePtr
      space = family.subsets.begin()->second.si->getStateSpace();
   ompl::base::SpaceInformationPtr
      si(new ompl::base::SpaceInformation(space));
   ompl::base::StateValidityCheckerPtr
      si_checker(new ompl::base::AllValidStateValidityChecker(si));
   si->setStateValidityChecker(si_checker);
   si->setup();
   return si;
}

} // anonymous namespace


ompl_multiset::FamilyPlanner::FamilyPlanner(
      const Family & family,
      const RoadmapGenPtr roadmap_gen,
      std::ostream & os_graph, std::ostream & os_alglog,
      int num_subgraphs):
   ompl::base::Planner(get_bogus_si(family), "FamilyPlanner"),
   family_effort_model(family),
   roadmap_gen(roadmap_gen),
   space(si_->getStateSpace()),
   check_radius(0.5*space->getLongestValidSegmentLength()),
   os_graph(os_graph), os_alglog(os_alglog),
   eig(g, get(&EProps::index,g)),
   overlay_manager(eig,og,
      get(&OverVProps::core_vertex, og),
      get(&OverEProps::core_edge, og)),
   num_subgraphs(num_subgraphs),
   coeff_checkcost(0.),
   coeff_distance(1.),
   coeff_subgraph(0.)
{
   // before we start,
   // generate some levels into our core eraph
   // note that new vertices/edges get properties from constructor
   roadmap_gen->generate(eig, num_subgraphs,
      get(&VProps::state, g),
      get(&EProps::distance, g),
      get(&VProps::subgraph, g),
      get(&EProps::subgraph, g),
      get(&VProps::is_shadow, g));
   
   // initialize stuff
   EdgeIter ei, ei_end;
   for (boost::tie(ei,ei_end)=edges(g); ei!=ei_end; ++ei)
   {
      edge_init_points(
         g[source(*ei,g)].state->state,
         g[target(*ei,g)].state->state,
         g[*ei].distance,
         g[*ei].edge_states);
      g[*ei].edge_tags.resize(g[*ei].edge_states.size(), 0);
      //g[*ei].tag = 0;
   }
}

ompl_multiset::FamilyPlanner::~FamilyPlanner()
{
}

void ompl_multiset::FamilyPlanner::setProblemDefinition(
   const ompl::base::ProblemDefinitionPtr & pdef)
{
   // call planner base class implementation
   // this will set my pdef_ and update my pis_
   ompl::base::Planner::setProblemDefinition(pdef);
   
   ompl::base::SpaceInformationPtr si_new = pdef->getSpaceInformation();
   if (si_new != family_effort_model.si_target)
   {
      // route target si to family effort model
      // this will re-run reverse dijkstra's on the family graph
      family_effort_model.set_target(si_new);
      
      // recalculate wlazy if necessary
      // we probably always need to recalc wlazy for overlay states!
      EdgeIter ei, ei_end;
      for (boost::tie(ei,ei_end)=edges(g); ei!=ei_end; ++ei)
         calculate_w_lazy(*ei);
   }
   
   overlay_unapply();
   
   // clear overlay graph
   og.clear();
   
   // add start to overlay graph
   assert(pdef->getStartStateCount() == 1);
   ov_start = add_vertex(og);
   og[ov_start].core_vertex = boost::graph_traits<Graph>::null_vertex();
   // set state
   og[ov_start].state.reset(new StateCon(space.get()));
   space->copyState(og[ov_start].state->state, pdef->getStartState(0));
   // regular vertex properties
   og[ov_start].subgraph = 0;
   og[ov_start].is_shadow = false;
   og[ov_start].tag = 0;

   // add goal to overlay graph
   ompl::base::GoalPtr goal = pdef->getGoal();
   assert(goal->hasType(ompl::base::GOAL_STATE));
   ompl::base::GoalState * goal_state = goal->as<ompl::base::GoalState>();
   ov_goal = add_vertex(og);
   og[ov_goal].core_vertex = boost::graph_traits<Graph>::null_vertex();
   // set state
   og[ov_goal].state.reset(new StateCon(space.get()));
   space->copyState(og[ov_goal].state->state, goal_state->getState());
   // regular vertex properties
   og[ov_goal].subgraph = 0;
   og[ov_goal].is_shadow = false;
   og[ov_start].tag = 0;
   
   // connect to vertices within fixed radius in roadmap
   std::vector<OverVertex> ovs;
   ovs.push_back(ov_start);
   ovs.push_back(ov_goal);
   for (std::vector<OverVertex>::iterator it=ovs.begin(); it!=ovs.end(); it++)
   {
      VertexIter vi, vi_end;
      for (boost::tie(vi,vi_end)=vertices(g); vi!=vi_end; ++vi)
      {
         double dist = space->distance(
            og[*it].state->state,
            g[*vi].state->state);
         if (0.12 < dist)
            continue;
         
         // add new anchor overlay vertex
         OverVertex v_anchor = add_vertex(og);
         og[v_anchor].core_vertex = *vi;
         // no need to set core properties (e.g. state) on anchors,
         // since this is just an anchor, wont be copied

         // add overlay edge from root to anchor
         OverEdge e = add_edge(*it, v_anchor, og).first;
         // add edge properties
         // og[e].core_properties.index -- needs to be set on apply
         og[e].distance = dist;
         og[e].subgraph = 0;
         // w_lazy??
         // interior points, in bisection order
         edge_init_points(og[*it].state->state, g[*vi].state->state,
            dist, og[e].edge_states);
         og[e].edge_tags.resize(og[e].edge_states.size(), 0);
         //og[e].tag = 0;
      }
   }
   
   overlay_apply();
}

ompl::base::PlannerStatus
ompl_multiset::FamilyPlanner::solve(
   const ompl::base::PlannerTerminationCondition & ptc)
{
   // ok, do some sweet sweet lazy search!
   
   bool success = false;
   std::vector<Edge> epath;
   
   // run batches of lazy search
   while (!success)
   {
      //os_alglog << "search_with_subgraphs " << num_subgraphs << std::endl;
      
      os_alglog << "alias reset" << std::endl;
      
      for (unsigned int ui=0; ui<overlay_manager.applied_vertices.size(); ui++)
      {
         OverVertex vover = overlay_manager.applied_vertices[ui];
         Vertex vcore = og[vover].core_vertex;
         os_alglog << "alias vertex applied-" << ui
            << " index " << get(get(boost::vertex_index,g),vcore) << std::endl;
      }
      
      for (unsigned int ui=0; ui<overlay_manager.applied_edges.size(); ui++)
      {
         OverEdge eover = overlay_manager.applied_edges[ui];
         Edge ecore = og[eover].core_edge;
         os_alglog << "alias edge applied-" << ui
            << " index " << g[ecore].index << std::endl;
      }
      
      // run lazy search
      success = pr_bgl::lazy_shortest_path(g,
         og[ov_start].core_vertex,
         og[ov_goal].core_vertex,
         ompl_multiset::WMap(*this),
         get(&EProps::w_lazy,g),
         ompl_multiset::IsEvaledMap(*this),
         epath,
         //pr_bgl::LazySpEvalFwd(),
         pr_bgl::LazySpEvalAlt(),
         ompl_multiset::make_lazysp_log_visitor(
            get(boost::vertex_index, g),
            get(&EProps::index, g),
            os_alglog));
      
      if (success)
         break;
      
      if (roadmap_gen->num_subgraphs && roadmap_gen->num_subgraphs < num_subgraphs + 1)
      {
         break;
      }
      
      printf("densifying ...\n");
      
      overlay_unapply();
      
      num_subgraphs++;
      
      size_t num_edges_before = num_edges(g);
      
      // add a subgraph!
      roadmap_gen->generate(eig, num_subgraphs,
         get(&VProps::state, g),
         get(&EProps::distance, g),
         get(&VProps::subgraph, g),
         get(&EProps::subgraph, g),
         get(&VProps::is_shadow, g));
         
      // initialize NEW edges
      EdgeIter ei, ei_end;
      for (boost::tie(ei,ei_end)=edges(g); ei!=ei_end; ++ei)
      {
         if (g[*ei].index < num_edges_before)
            continue;
         edge_init_points(
            g[source(*ei,g)].state->state,
            g[target(*ei,g)].state->state,
            g[*ei].distance,
            g[*ei].edge_states);
         g[*ei].edge_tags.resize(g[*ei].edge_states.size(), 0);
         //g[*ei].tag = 0;
         calculate_w_lazy(*ei);
      }
      
      overlay_apply();
   }
   
   // dump graph
   // write it out to file
   pr_bgl::GraphIO<Graph, VertexIndexMap, EdgeIndexMap, EdgeVectorMap>
      io(g,
         get(boost::vertex_index, g),
         get(&EProps::index, g),
         eig.edge_vector_map);
   io.add_property_map("state", pr_bgl::make_string_map(get(&VProps::state,g)));
   io.add_property_map("subgraph", pr_bgl::make_string_map(get(&VProps::subgraph,g)));
   io.add_property_map("is_shadow", pr_bgl::make_string_map(get(&VProps::is_shadow,g)));
   io.add_property_map("subgraph", pr_bgl::make_string_map(get(&EProps::subgraph,g)));
   io.add_property_map("distance", pr_bgl::make_string_map(get(&EProps::distance,g)));
   io.dump_graph(os_graph);
   io.dump_properties(os_graph);
   
   if (success)
   {
      /* create the path */
      ompl::geometric::PathGeometric * path
         = new ompl::geometric::PathGeometric(si_);
      path->append(g[og[ov_start].core_vertex].state->state);
      for (std::vector<Edge>::iterator it=epath.begin(); it!=epath.end(); it++)
         path->append(g[target(*it,g)].state->state);
      pdef_->addSolutionPath(ompl::base::PathPtr(path));
      return ompl::base::PlannerStatus::EXACT_SOLUTION;
   }
   else
   {
      return ompl::base::PlannerStatus::TIMEOUT;
   }
}

void ompl_multiset::FamilyPlanner::overlay_apply()
{
   if (overlay_manager.is_applied)
      return;
   
   overlay_manager.apply();
   
   // manually copy over properties
   for (unsigned int ui=0; ui<overlay_manager.applied_vertices.size(); ui++)
   {
      OverVertex vover = overlay_manager.applied_vertices[ui];
      Vertex vcore = og[vover].core_vertex;
      g[vcore].state = og[vover].state;
      g[vcore].subgraph = og[vover].subgraph;
      g[vcore].is_shadow = og[vover].is_shadow;
      g[vcore].tag = og[vover].tag;
   }
   
   for (unsigned int ui=0; ui<overlay_manager.applied_edges.size(); ui++)
   {
      OverEdge eover = overlay_manager.applied_edges[ui];
      Edge ecore = og[eover].core_edge;
      g[ecore].distance = og[eover].distance;
      g[ecore].subgraph = og[eover].subgraph;
      //g[ecore].w_lazy = og[eover].w_lazy;
      g[ecore].edge_states = og[eover].edge_states;
      g[ecore].edge_tags = og[eover].edge_tags;
      //g[ecore].tag = og[eover].tag;
      calculate_w_lazy(ecore);
   }
}

void ompl_multiset::FamilyPlanner::overlay_unapply()
{
   // unapply the overlay graph if it is applied
   if (!overlay_manager.is_applied)
      return;
   
   for (unsigned int ui=0; ui<overlay_manager.applied_vertices.size(); ui++)
   {
      OverVertex vover = overlay_manager.applied_vertices[ui];
      Vertex vcore = og[vover].core_vertex;
      og[vover].state = g[vcore].state;
      og[vover].subgraph = g[vcore].subgraph;
      og[vover].is_shadow = g[vcore].is_shadow;
      og[vover].tag = g[vcore].tag;
   }
   
   for (unsigned int ui=0; ui<overlay_manager.applied_edges.size(); ui++)
   {
      OverEdge eover = overlay_manager.applied_edges[ui];
      Edge ecore = og[eover].core_edge;
      og[eover].distance = g[ecore].distance;
      og[eover].subgraph = g[ecore].subgraph;
      //og[eover].w_lazy = g[ecore].w_lazy;
      //og[eover].is_evaled = g[ecore].is_evaled;
      og[eover].edge_states = g[ecore].edge_states;
      og[eover].edge_tags = g[ecore].edge_tags;
      //og[eover].tag = g[ecore].tag;
   }
   
   overlay_manager.unapply();
}

void ompl_multiset::FamilyPlanner::edge_init_points(
   ompl::base::State * va_state, ompl::base::State * vb_state,
   double e_distance, std::vector< boost::shared_ptr<StateCon> > & edge_states)
{
   // now many interior points do we need?
   unsigned int n = floor(e_distance/(2.0*check_radius));
   // allocate states
   edge_states.resize(n);
   for (unsigned int ui=0; ui<n; ui++)
      edge_states[ui].reset(new StateCon(space.get()));
   // fill with interpolated states in bisection order
   const std::vector< std::pair<int,int> > & order = bisect_perm.get(n);
   for (unsigned int ui=0; ui<n; ui++)
      space->interpolate(va_state, vb_state,
         1.0*(1+order[ui].first)/(n+1),
         edge_states[ui]->state);
}

void ompl_multiset::FamilyPlanner::calculate_w_lazy(const Edge & e)
{
   unsigned int ui;
   for (ui=0; ui<g[e].edge_tags.size(); ui++)
      if (family_effort_model.x_hat(g[e].edge_tags[ui]) == std::numeric_limits<double>::infinity())
         break;
   if (ui<g[e].edge_states.size()
      || family_effort_model.x_hat(g[source(e,g)].tag) == std::numeric_limits<double>::infinity()
      || family_effort_model.x_hat(g[target(e,g)].tag) == std::numeric_limits<double>::infinity())
   {
      g[e].w_lazy = std::numeric_limits<double>::infinity();
   }
   else
   {
      g[e].w_lazy = 0.0;
      g[e].w_lazy += coeff_distance * g[e].distance;
      g[e].w_lazy += coeff_subgraph * g[e].distance * g[e].subgraph;
      // interior states
      for (ui=0; ui<g[e].edge_tags.size(); ui++)
         g[e].w_lazy += coeff_checkcost * family_effort_model.p_hat(g[e].edge_tags[ui]);
      // half bounary vertices
      g[e].w_lazy += 0.5 * coeff_checkcost * family_effort_model.p_hat(g[source(e,g)].tag);
      g[e].w_lazy += 0.5 * coeff_checkcost * family_effort_model.p_hat(g[target(e,g)].tag);
   }
}

bool ompl_multiset::FamilyPlanner::isevaledmap_get(const Edge & e)
{
   // this directly calls the family effort model (distance not needed!)
   for (unsigned int ui=0; ui<g[e].edge_tags.size(); ui++)
      if (!family_effort_model.is_evaled(g[e].edge_tags[ui]))
         return false;
   return true;
}

double ompl_multiset::FamilyPlanner::wmap_get(const Edge & e)
{
   // check all points!   
   Vertex va = source(e, g);
   Vertex vb = target(e, g);
   
   // check endpoints first
   do
   {
      if (!family_effort_model.is_evaled(g[va].tag))
      {
         bool success = family_effort_model.eval_partial(g[va].tag, g[va].state->state);
         if (!success)
            break;
      }
      if (!family_effort_model.is_evaled(g[vb].tag))
      {
         bool success = family_effort_model.eval_partial(g[vb].tag, g[vb].state->state);
         if (!success)
            break;
      }
      for (unsigned ui=0; ui<g[e].edge_tags.size(); ui++)
      {
         if (!family_effort_model.is_evaled(g[e].edge_tags[ui]))
         {
            bool success = family_effort_model.eval_partial(g[e].edge_tags[ui], g[e].edge_states[ui]->state);
            if (!success)
               break;
         }
      }
   }
   while (0);
   
   // recalculate wlazy for this edge and any incident edges
   calculate_w_lazy(e);
   OutEdgeIter ei, ei_end;
   for (boost::tie(ei,ei_end)=out_edges(va,g); ei!=ei_end; ei++)
      calculate_w_lazy(*ei);
   for (boost::tie(ei,ei_end)=out_edges(vb,g); ei!=ei_end; ei++)
      calculate_w_lazy(*ei);
   
   return g[e].w_lazy;
}
