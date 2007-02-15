// **********************************************************************
//
// Copyright (c) 2003-2007 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <IceUtil/DisableWarnings.h>
#include <IceUtil/Options.h>
#include <IceUtil/CtrlCHandler.h>
#include <IceUtil/Thread.h>
#include <Ice/Ice.h>
#include <Ice/SliceChecksums.h>
#include <IceGrid/Parser.h>
#include <IceGrid/FileParserI.h>
#include <IceGrid/Registry.h>
#include <Glacier2/Router.h>
#include <fstream>

using namespace std;
using namespace Ice;
using namespace IceGrid;

class Client;

static IceUtil::StaticMutex _staticMutex = ICE_STATIC_MUTEX_INITIALIZER;
static Client* _globalClient = 0;

class SessionKeepAliveThread : public IceUtil::Thread, public IceUtil::Monitor<IceUtil::Mutex>
{
public:

    SessionKeepAliveThread(const AdminSessionPrx& session, long timeout) :
        _session(session),
        _timeout(IceUtil::Time::seconds(timeout)),
        _destroy(false)
    {
    }

    virtual void
    run()
    {
        Lock sync(*this);
        while(!_destroy)
        {
            timedWait(_timeout);
            if(_destroy)
            {
                break;
            }
            try
            {
                _session->keepAlive();
            }
            catch(const Ice::Exception&)
            {
                break;
            }
        }
    }

    void
    destroy()
    {
        Lock sync(*this);
        _destroy = true;
        notify();
    }

private:

    AdminSessionPrx _session;
    const IceUtil::Time _timeout;
    bool _destroy;
};
typedef IceUtil::Handle<SessionKeepAliveThread> SessionKeepAliveThreadPtr;

class Client : public IceUtil::Monitor<IceUtil::Mutex>
{
public:

    void usage();
    int main(int argc, char* argv[]);
    int run(int, char*[]);
    void interrupted();

    Ice::CommunicatorPtr communicator() const { return _communicator; }
    const char* appName() const { return _appName; }

    string trim(const string&);
private:

    IceUtil::CtrlCHandler _ctrlCHandler;
    Ice::CommunicatorPtr _communicator;
    const char* _appName;
    ParserPtr _parser;
};

static void interruptCallback(int signal)
{
    IceUtil::StaticMutex::Lock lock(_staticMutex);
    if(_globalClient)
    {
        _globalClient->interrupted();
    }
}

int
main(int argc, char* argv[])
{
    Client app;
    return app.main(argc, argv);
}

void
Client::usage()
{
    cerr << "Usage: " << appName() << " [options] [file...]\n";
    cerr <<     
        "Options:\n"
        "-h, --help           Show this message.\n"
        "-v, --version        Display the Ice version.\n"
        "-DNAME               Define NAME as 1.\n"
        "-DNAME=DEF           Define NAME as DEF.\n"
        "-UNAME               Remove any definition for NAME.\n"
        "-IDIR                Put DIR in the include file search path.\n"
        "-e COMMANDS          Execute COMMANDS.\n"
        "-d, --debug          Print debug messages.\n"
        "-s, --server         Start icegridadmin as a server (to parse XML files).\n"
        "-u, --username       Login with the given username.\n"
        "-p, --password       Login with the given password.\n"
        "-S, --ssl            Authenticate through SSL.\n"
        "-r, --routed         Login through a Glacier2 router.\n"
        "-R, --replica NAME   Connect to the replica NAME.\n"
        ;
}

int
Client::main(int argc, char* argv[])
{
    int status = EXIT_SUCCESS;

    try
    {
        _appName = argv[0];
        _communicator = Ice::initialize(argc, argv);
        
        {
            IceUtil::StaticMutex::Lock sync(_staticMutex);
            _globalClient = this;
        }
        _ctrlCHandler.setCallback(interruptCallback);

        try
        {
            run(argc, argv);
        }
        catch(const Ice::CommunicatorDestroyedException&)
        {
            // Expected if the client is interrupted during the initialization.
        }
    }
    catch(const IceUtil::Exception& ex)
    {
        cerr << _appName << ": " << ex << endl;
        status = EXIT_FAILURE;
    }
    catch(const std::exception& ex)
    {
        cerr << _appName << ": std::exception: " << ex.what() << endl;
        status = EXIT_FAILURE;
    }
    catch(const std::string& msg)
    {
        cerr << _appName << ": " << msg << endl;
        status = EXIT_FAILURE;
    }
    catch(const char* msg)
    {
        cerr << _appName << ": " << msg << endl;
        status = EXIT_FAILURE;
    }
    catch(...)
    {
        cerr << _appName << ": unknown exception" << endl;
        status = EXIT_FAILURE;
    }

    if(_communicator)
    {
        try
        {
            _communicator->destroy();
        }
        catch(const Ice::CommunicatorDestroyedException&)
        {
        }
        catch(const Ice::Exception& ex)
        {
            cerr << ex << endl;
            status = EXIT_FAILURE;
        }
    }

    _ctrlCHandler.setCallback(0);
    {
        IceUtil::StaticMutex::Lock sync(_staticMutex);
        _globalClient = 0;
    }

    return status;
        
}

void
Client::interrupted()
{
    Lock sync(*this);
    if(_parser) // If there's an interactive parser, notify the parser.
    {
        _parser->interrupt();
    }
    else
    {
        //
        // Otherwise, destroy the communicator.
        //
        assert(_communicator);
        try
        {
            _communicator->destroy();
        }
        catch(const Ice::Exception&)
        {
        }
    }
}

int
Client::run(int argc, char* argv[])
{
    string cpp("cpp");
    string commands;
    bool debug;

    IceUtil::Options opts;
    opts.addOpt("h", "help");
    opts.addOpt("v", "version");
    opts.addOpt("D", "", IceUtil::Options::NeedArg, "", IceUtil::Options::Repeat);
    opts.addOpt("U", "", IceUtil::Options::NeedArg, "", IceUtil::Options::Repeat);
    opts.addOpt("I", "", IceUtil::Options::NeedArg, "", IceUtil::Options::Repeat);
    opts.addOpt("e", "", IceUtil::Options::NeedArg, "", IceUtil::Options::Repeat);
    opts.addOpt("u", "username", IceUtil::Options::NeedArg, "", IceUtil::Options::NoRepeat);
    opts.addOpt("p", "password", IceUtil::Options::NeedArg, "", IceUtil::Options::NoRepeat);
    opts.addOpt("S", "ssl");
    opts.addOpt("r", "routed");
    opts.addOpt("d", "debug");
    opts.addOpt("s", "server");
    opts.addOpt("R", "replica", IceUtil::Options::NeedArg, "", IceUtil::Options::NoRepeat);

    vector<string> args;
    try
    {
        args = opts.parse(argc, (const char**)argv);
    }
    catch(const IceUtil::BadOptException& e)
    {
        cerr << e.reason << endl;
        usage();
        return EXIT_FAILURE;
    }

    if(opts.isSet("help"))
    {
        usage();
        return EXIT_SUCCESS;
    }
    if(opts.isSet("version"))
    {
        cout << ICE_STRING_VERSION << endl;
        return EXIT_SUCCESS;
    }

    if(opts.isSet("server"))
    {
        ObjectAdapterPtr adapter = communicator()->createObjectAdapterWithEndpoints("FileParser", "tcp -h localhost");
        adapter->activate();
        ObjectPrx proxy = adapter->add(new FileParserI, communicator()->stringToIdentity("FileParser"));
        cout << proxy << endl;

        communicator()->waitForShutdown();
        return EXIT_SUCCESS;
    }

    if(opts.isSet("D"))
    {
        vector<string> optargs = opts.argVec("D");
        for(vector<string>::const_iterator i = optargs.begin(); i != optargs.end(); ++i)
        {
            cpp += " -D" + *i;
        }
    }
    if(opts.isSet("U"))
    {
        vector<string> optargs = opts.argVec("U");
        for(vector<string>::const_iterator i = optargs.begin(); i != optargs.end(); ++i)
        {
            cpp += " -U" + *i;
        }
    }
    if(opts.isSet("I"))
    {
        vector<string> optargs = opts.argVec("I");
        for(vector<string>::const_iterator i = optargs.begin(); i != optargs.end(); ++i)
        {
            cpp += " -I" + *i;
        }
    }
    if(opts.isSet("e"))
    {
        vector<string> optargs = opts.argVec("e");
        for(vector<string>::const_iterator i = optargs.begin(); i != optargs.end(); ++i)
        {
            commands += *i + ";";
        }
    }
    debug = opts.isSet("debug");

    if(!args.empty() && !commands.empty())
    {
        cerr << appName() << ": `-e' option cannot be used if input files are given" << endl;
        usage();
        return EXIT_FAILURE;
    }

    string instanceName;
    if(communicator()->getDefaultLocator())
    {
        instanceName = communicator()->getDefaultLocator()->ice_getIdentity().category; 
    }
    else
    {
        instanceName = communicator()->getProperties()->getPropertyWithDefault("IceGrid.InstanceName", "IceGrid");
    }
    
    bool ssl = communicator()->getProperties()->getPropertyAsInt("IceGridAdmin.AuthenticateUsingSSL");
    if(opts.isSet("ssl"))
    {
        ssl = true;
    }

    string id = communicator()->getProperties()->getProperty("IceGridAdmin.Username");
    if(!opts.optArg("username").empty())
    {
        id = opts.optArg("username");
    }
    string password = communicator()->getProperties()->getProperty("IceGridAdmin.Password");
    if(!opts.optArg("password").empty())
    {
        password = opts.optArg("password");
    }

    //
    // If a glacier2 router is configured, then set routed to true by
    // default.
    //
    Ice::PropertiesPtr properties = communicator()->getProperties();
    bool routed = properties->getPropertyAsIntWithDefault("IceGridAdmin.Routed", communicator()->getDefaultRouter());
    if(opts.isSet("routed"))
    {
        routed = true;
    }
    string replica = properties->getProperty("IceGridAdmin.Replica");
    if(!opts.optArg("replica").empty())
    {
        replica = opts.optArg("replica");
    }

    AdminSessionPrx session;
    SessionKeepAliveThreadPtr keepAlive;
    int status = EXIT_SUCCESS;
    try
    {
        int timeout;
        if(routed)
        {
            Glacier2::RouterPrx router = Glacier2::RouterPrx::checkedCast(communicator()->getDefaultRouter());
            if(!router)
            {
                cerr << argv[0] << ": configured router is not a Glacier2 router" << endl;
                return EXIT_FAILURE;
            }

            // Use SSL if available.
            try
            {
                router = Glacier2::RouterPrx::checkedCast(router->ice_secure(true));
            }
            catch(const Ice::NoEndpointException&)
            {
            }

            if(ssl)
            {
                session = AdminSessionPrx::uncheckedCast(router->createSessionFromSecureConnection());
                if(!session)
                {
                    cerr << argv[0]
                         << ": Glacier2 returned a null session, please set the Glacier2.SSLSessionManager property"
                         << endl;
                    return EXIT_FAILURE;
                }
            }
            else
            {
                while(id.empty())
                {
                    cout << "user id: " << flush;
                    getline(cin, id);
                    id = trim(id);
                }
                
                if(password.empty())
                {
                    cout << "password: " << flush;
                    getline(cin, password);
                    password = trim(password);
                }
                    
                session = AdminSessionPrx::uncheckedCast(router->createSession(id, password));
                if(!session)
                {
                    cerr << argv[0]
                         << ": Glacier2 returned a null session, please set the Glacier2.SessionManager property"
                         << endl;
                    return EXIT_FAILURE;
                }
            }
            timeout = static_cast<int>(router->getSessionTimeout());
        }
        else
        {
            Identity registryId;
            registryId.category = instanceName;
            registryId.name = "Registry";
            if(!replica.empty() && replica != "Master")
            {
                registryId.name += "-" + replica;
            }

            RegistryPrx registry;
            try
            {
                registry = RegistryPrx::checkedCast(
                        communicator()->stringToProxy("\"" + communicator()->identityToString(registryId) + "\""));
                if(!registry)
                {
                    cerr << argv[0] << ": could not contact registry" << endl;
                    return EXIT_FAILURE;
                }
            }
            catch(const Ice::NotRegisteredException&)
            {
                cerr << argv[0] << ": no active registry replica named `" << replica << "'" << endl;
                return EXIT_FAILURE;            
            }

            // Use SSL if available.
            try
            {
                registry = RegistryPrx::checkedCast(registry->ice_secure(true));
            }
            catch(const Ice::NoEndpointException&)
            {
            }

            if(ssl)
            {
                session = registry->createAdminSessionFromSecureConnection();
            }
            else
            {
                while(id.empty())
                {
                    cout << "user id: " << flush;
                    getline(cin, id);
                    id = trim(id);
                }
                
                if(password.empty())
                {
                    cout << "password: " << flush;
                    getline(cin, password);
                    password = trim(password);
                }
                    
                session = registry->createAdminSession(id, password);
            }
            assert(session);
            timeout = registry->getSessionTimeout();
        }
        
        keepAlive = new SessionKeepAliveThread(session, timeout / 2);
        keepAlive->start();

        AdminPrx admin = session->getAdmin();

        Ice::SliceChecksumDict serverChecksums = admin->getSliceChecksums();
        Ice::SliceChecksumDict localChecksums = Ice::sliceChecksums();

        //
        // The following slice types are only used by the admin CLI.
        //
        localChecksums.erase("::IceGrid::FileParser");
        localChecksums.erase("::IceGrid::ParseException");
                         
        for(Ice::SliceChecksumDict::const_iterator q = localChecksums.begin(); q != localChecksums.end(); ++q)
        {
            Ice::SliceChecksumDict::const_iterator r = serverChecksums.find(q->first);
            if(r == serverChecksums.end())
            {
                cerr << appName() << ": server is using unknown Slice type `" << q->first << "'" << endl;
            }
            else if(q->second != r->second)
            {
                cerr << appName() << ": server is using a different Slice definition of `" << q->first << "'" << endl;
            }
        }

        {
            Lock sync(*this);
            _parser = Parser::createParser(communicator(), session, admin, args.empty() && commands.empty());
        }    

        if(!args.empty()) // Files given
        {
            // Process files given on the command line
            for(vector<string>::const_iterator i = args.begin(); i != args.end(); ++i)
            {
                ifstream test(i->c_str());
                if(!test)
                {
                    cerr << appName() << ": can't open `" << *i << "' for reading: " << strerror(errno) << endl;
                    return EXIT_FAILURE;
                }
                test.close();
            
                string cmd = cpp + " " + *i;
#ifdef _WIN32
                FILE* cppHandle = _popen(cmd.c_str(), "r");
#else
                FILE* cppHandle = popen(cmd.c_str(), "r");
#endif
                if(cppHandle == NULL)
                {
                    cerr << appName() << ": can't run C++ preprocessor: " << strerror(errno) << endl;
                    return EXIT_FAILURE;
                }
            
                int parseStatus = _parser->parse(cppHandle, debug);
            
#ifdef _WIN32
                _pclose(cppHandle);
#else
                pclose(cppHandle);
#endif

                if(parseStatus == EXIT_FAILURE)
                {
                    status = EXIT_FAILURE;
                }
            }
        }
        else if(!commands.empty()) // Commands were given
        {
            int parseStatus = _parser->parse(commands, debug);
            if(parseStatus == EXIT_FAILURE)
            {
                status = EXIT_FAILURE;
            }
        }
        else // No commands, let's use standard input
        {
            _parser->showBanner();
            
            int parseStatus = _parser->parse(stdin, debug);
            if(parseStatus == EXIT_FAILURE)
            {
                status = EXIT_FAILURE;
            }
        }
    }
    catch(const IceGrid::PermissionDeniedException& ex)
    {
        cout << "permission denied:\n" << ex.reason << endl;
        return EXIT_FAILURE;
    }
    catch(...)
    {
        if(keepAlive)
        {
            keepAlive->destroy();
            keepAlive->getThreadControl().join();
        }

        if(session)
        {
            try
            {
                session->destroy();
            }
            catch(const Ice::Exception&)
            {
            }
        }
        throw;
    }

    keepAlive->destroy();
    keepAlive->getThreadControl().join();

    if(session)
    {
        try
        {
            session->destroy();
        }
        catch(const Ice::Exception&)
        {
            // Ignore. If the registry has been shutdown this will cause
            // an exception.
        }
    }

    return status;
}

string
Client::trim(const string& s)
{
    static const string delims = "\t\r\n ";
    string::size_type last = s.find_last_not_of(delims);
    if(last != string::npos)
    {
        return s.substr(s.find_first_not_of(delims), last+1);
    }
    return s;
}
