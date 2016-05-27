#include "ParseCommand.h"

CommandLineParserClass::CommandLineParserClass(std::list<ParamClass> &argList)
{
  std::list<ParamClass>::iterator iter;
  for (iter=argList.begin(); iter!=argList.end(); iter++) 
    ArgMap[iter->GetName()] = (*iter);
}

bool
CommandLineParserClass::Parse(int argc, char **argv)
{
  for (int i=1; i<argc; i++) {
    if ((argv[i][0]=='-') && (argv[i][1]=='-')) {
      std::string name = &(argv[i][2]);
      std::map<std::string, ParamClass>::iterator iter;
      iter = ArgMap.find(name);
      if (iter != ArgMap.end()) {
	ArgMap[name].Found = true;
	for (int j=0; j<ArgMap[name].NumArgsNeeded; j++)
	  if ((i+1) < argc) 
	    ArgMap[name].SetArg(argv[++i]);
	  else
	    return false;
      }
      else {
	std::cerr << "Unrecognized argument """ << name << """" << std::endl;
	return false;
      }
    }
    else if (argv[i][0] == '-')
      return false;
    else 
      Files.push_back (argv[i]);
  }
  return true;
}
