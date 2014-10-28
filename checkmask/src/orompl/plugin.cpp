#include <openrave/openrave.h>
//#include <openrave/planner.h>
#include <openrave/plugin.h>

//#include <ompl/base/spaces/RealVectorStateSpace.h>
//#include <ompl/base/Planner.h>
//#include <ompl/base/ProblemDefinition.h>
//#include <ompl/base/ScopedState.h>
//#include <ompl/base/SpaceInformation.h>

#include "planner_checkmask.h"

void GetPluginAttributesValidated(OpenRAVE::PLUGININFO& info)
{
   info.interfacenames[OpenRAVE::PT_Planner].push_back("OmplCheckMask");
}

OpenRAVE::InterfaceBasePtr CreateInterfaceValidated(
   OpenRAVE::InterfaceType type,
   const std::string & interfacename,
   std::istream& sinput,
   OpenRAVE::EnvironmentBasePtr penv)
{
   if((type == OpenRAVE::PT_Planner) && (interfacename == "omplcheckmask"))
      return OpenRAVE::InterfaceBasePtr(new checkmask::OmplCheckMask(penv));
   return OpenRAVE::InterfaceBasePtr();
}

OPENRAVE_PLUGIN_API void DestroyPlugin()
{
}
