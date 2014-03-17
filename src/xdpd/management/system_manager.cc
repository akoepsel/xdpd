#include "system_manager.h"

#include <sstream>
#include <rofl/common/ciosrv.h>
#include <rofl/common/utils/c_logger.h>
#include <rofl/platform/unix/cdaemon.h>
#include "switch_manager.h"
#include "port_manager.h"
#include "plugin_manager.h"


using namespace xdpd;
using namespace rofl;

//getopt
extern int optind;

/*
* Static member initialization
*/
bool system_manager::inited = false;
fwd_module_info_t system_manager::info;
rofl::cunixenv* system_manager::env_parser = NULL;
const std::string system_manager::XDPD_CLOG_FILE="./xdpd.log";
const std::string system_manager::XDPD_LOG_FILE="/var/log/xdpd.log";
const std::string system_manager::XDPD_PID_FILE="/var/run/xdpd.pid";
const unsigned int system_manager::XDPD_DEFAULT_DEBUG_LEVEL=5; //NOTICE

const std::string system_manager::XDPD_TEST_RUN_OPT_FULL_NAME="test-config";
const std::string system_manager::XDPD_EXTRA_PARAMS_OPT_FULL_NAME="extra-params";


//Handler to stop ciosrv
void interrupt_handler(int dummy=0) {
	//Only stop ciosrv 
	ciosrv::stop();
}


std::string system_manager::get_driver_extra_params(){

	std::string extra="";
	std::string extra_plugins;

	//First recover the CL option if exists
	if(is_option_set(XDPD_EXTRA_PARAMS_OPT_FULL_NAME))
		extra = get_option_value(XDPD_EXTRA_PARAMS_OPT_FULL_NAME);

	//Recover plugins extra
	extra_plugins = plugin_manager::__get_driver_extra_params();

	if(extra_plugins != ""){
		if(extra != ""){
			//Notify user
			ROFL_ERR("[xdpd][system] Warning: Ignoring extra driver parameters provided by plugins (%s), since xDPd was launched with -e (%s)\n", extra_plugins.c_str(), extra.c_str());
		}else
			extra = extra_plugins;
	}

	return extra;
}

//Set the debug level
void system_manager::set_logging_debug_level(unsigned int level){

	enum rofl_debug_levels c_level;
	
	if( inited && env_parser->is_arg_set("debug") ){
		ROFL_ERR("[xdpd][system] Ignoring the attempt to set_logging_debug_level(); logging level set via command line has preference.\n");
		throw eSystemLogLevelSetviaCL(); 
	}

	//Validate level and map to C
	switch(level){
		case logging::EMERG:
		case logging::ALERT:
		case logging::CRIT:
		case logging::ERROR: c_level = ERROR_LEVEL;
			break;
		case logging::WARN: c_level = WARN_LEVEL;
			break;
		case logging::NOTICE:
		case logging::INFO: c_level = INFO_LEVEL;
			break;
		case logging::DBG: c_level = DBG_LEVEL;
			break;
		case logging::TRACE: c_level = DBG_VERBOSE_LEVEL;
			break;
		default:
			throw eSystemLogInvalidLevel(); 
	}
	//Adjust C++ logging debug level	
	logging::set_debug_level(level);
	
	//Adjust C logging debug level
	rofl_set_logging_level(/*cn,*/c_level);
#if 0
	//FIXME translate C++ DEBUG level to C
	#if DEBUG && VERBOSE_DEBUG
		//Set verbose debug if necessary
		rofl_set_logging_level(/*cn,*/ DBG_VERBOSE_LEVEL);
	#else
		//XXX
	#endif
#endif
}

void system_manager::init_command_line_options(){

	//Daemonize
	env_parser->add_option(coption(true, NO_ARGUMENT,'D',"daemonize","Daemonize execution",""));
	
	//Log file
	env_parser->add_option(coption(true,REQUIRED_ARGUMENT,'l',"logfile","Log file used when daemonization", XDPD_CLOG_FILE));
	
	//Extra forwarding module parameters
	env_parser->add_option(coption(true,REQUIRED_ARGUMENT, 'e', XDPD_EXTRA_PARAMS_OPT_FULL_NAME, "Quoted string of extra parameters that will be passed to the platform driver. Use -h to get the details of the particular options on your platform driver", ""));

	//Test
	env_parser->add_option(coption(true, NO_ARGUMENT, 't', XDPD_TEST_RUN_OPT_FULL_NAME, "Test configuration only and exit", ""));

	//Version
	env_parser->add_option(coption(true, NO_ARGUMENT, 'v', "version", "Retrieve xDPd version and exit", std::string("")));


	//Add plugin options
	std::vector<coption> plugin_options = plugin_manager::__get_plugin_options();
	
	//Add them 
	for(std::vector<coption>::iterator it = plugin_options.begin(); it != plugin_options.end(); ++it){
		env_parser->add_option(*it);
	}
}

void system_manager::init(int argc, char** argv){

	unsigned int debug_level;

	//Prevent double calls to init()
	if(inited)
		ROFL_ERR("[xdpd][system] ERROR: double call to system_amanager::init(). This can only be caused by a spurious call from a misbehaving plugin. Please notify this error. Continuing execution...\n");

	//Set forwarding module info cache
	fwd_module_get_info(&info);

	/* Parse arguments. Add first additional arguments */
	env_parser = new cunixenv(argc, argv);

	//Initialize command line options 
	init_command_line_options();
	
	//Parse arguments
	env_parser->parse_args();

	//If -v is set, print version and return immediately. Note that this must be here after
	//get_info 
	if(env_parser->is_arg_set("version")) {
		ROFL_INFO(get_version().c_str());
		goto SYSTEM_MANAGER_CLEANUP;
	}

	//Help
	if(env_parser->is_arg_set("help")) {
		dump_help();
		goto SYSTEM_MANAGER_CLEANUP;
	}
	
	//Set debugging
	debug_level = XDPD_DEFAULT_DEBUG_LEVEL;
	if(env_parser->is_arg_set("debug"))	
		debug_level = atoi(env_parser->get_arg("debug").c_str());
	set_logging_debug_level(debug_level);

	//Daemonize
	if(env_parser->is_arg_set("daemonize")) {
		rofl::cdaemon::daemonize(XDPD_PID_FILE, XDPD_LOG_FILE);
		rofl::logging::notice << "[xdpd][system] daemonizing successful" << std::endl;
	}

	//Capture control+C
	signal(SIGINT, interrupt_handler);
		
	if(is_test_run())
		rofl::logging::notice << "[xdpd][system] Launched with -t "<< XDPD_TEST_RUN_OPT_FULL_NAME <<". Doing a test-run execution" << std::endl;

	//Forwarding module initialization
	if(fwd_module_init(get_driver_extra_params().c_str()) != AFA_SUCCESS){
		ROFL_ERR("[xdpd][system] ERROR: initialization of platform driver failed! Aborting...\n");	
		exit(EXIT_FAILURE);
	}

	//Mark as initied. MUST BE here, after the first setting of the logging level has been
	//done
	inited = true;
	
	//Load plugins
	optind=0;
	plugin_manager::init();

	//If test-config is not set, launch ciosrv loop, otherwise terminate execution
	if(!is_test_run()){
		//ciosrv run. Only will stop in Ctrl+C
		ciosrv::run();
	}

	//Printing nice trace
	ROFL_INFO("\n[xdpd][system] Shutting down...\n");	

	//Destroy all state
	switch_manager::destroy_all_switches();

	//Call fwd_module to shutdown
	fwd_module_destroy();
	
	//Let plugin manager destroy all registered plugins
	//This must be after calling fwd_module_destroy()
	plugin_manager::destroy();
	
	//Logging	
	logging::close();

	//Release ciosrv loop resources
	rofl::cioloop::shutdown();

	//Print a nice trace
	ROFL_INFO("\n[xdpd][system] Shutted down.\n");

SYSTEM_MANAGER_CLEANUP:

	//Release cunixenv
	delete env_parser;	
}




//Dumps help
void system_manager::dump_help(){
	std::string xdpd_name="xdpd";
	ROFL_INFO("\n%s\n", env_parser->get_usage((char*)xdpd_name.c_str()).c_str());
}


//Generates the version string
std::string system_manager::get_version(){

	std::stringstream ss("");
	
	//xDPd CMM information
	ss << std::endl << "The eXtensible OpenFlow Datapath daemon (xDPd)" << std::endl;	
	ss << "Version: "<< XDPD_VERSION << std::endl;

#ifdef XDPD_BUILD
	ss << "Build: " << XDPD_BUILD << std::endl;
	ss << "Compiled in branch: " << XDPD_BRANCH << std::endl;
	ss << "Detailed build information:" << XDPD_DESCRIBE << std::endl;
#endif	

	//xDPd forwarding module information
	ss << "\n-- Hardware support (forwarding module) --" << std::endl;
	ss << "Driver code name: "<< info.code_name << std::endl;
	ss << "Driver version: "<< info.version << std::endl;
	ss << "Driver description: "<< info.description << std::endl;
	
	//Libraries info	
	ss << "\n-- Libraries --" << std::endl;
	ss << "[ROFL]" << std::endl;
	ss << "  Version: " << ROFL_VERSION << std::endl;
	ss << "  Build: " << ROFL_BUILD_NUM << std::endl;
	ss << "  Compiled in branch: " << ROFL_BUILD_BRANCH << std::endl;
	ss << "  Detailed build information:" << ROFL_BUILD_DESCRIBE << std::endl << std::endl;
	
	return ss.str();
}

//
// Options
//

bool system_manager::is_option_set(const std::string& option_name){
	return env_parser->is_arg_set(option_name);	
}

std::string system_manager::get_option_value(const std::string& option_name){
	return env_parser->get_arg(option_name);
}

