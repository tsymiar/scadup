#include "scadup/Scadup.h"
#include "utils/FileUtils.h"

#ifndef _WIN32
#include <unistd.h>
// #define _USE_FORK_PROCESS_
#endif
#include <iostream>

using namespace std;

static void usage()
{
    cout << "Usage:" << endl
        << "brk         -- run as broker" << endl
        << "sub [topic] -- run as subscriber" << endl
        << "pub [topic] [payload] -- run as publisher messaging to broker" << endl
        << "pub [topic] [-f [filename]] -- run as publisher send file content" << endl
        << "-S  -- run as server" << endl
        << "-C  -- run as client" << endl;
}

int main(int argc, char* argv[])
{
    G_MethodEnum method = NONE;
    if (argc > 1) {
        string argv1 = string(argv[1]);
        method = (argv1 == "-S" ? SERVER :
            (argv1 == "-C" ? CLIENT :
                (argv1 == "brk" ? BROKER :
                    (argv1 == "pub" ? PUBLISH :
                        (argv1 == "sub" ? SUBSCRIBE : NOTIMPL)))));
    } else {
        usage();
        return 0;
    }
#ifdef _USE_FORK_PROCESS_
    pid_t child = fork();
    if (child == 0) {
#endif
        Scadup scadup;
        unsigned short PORT = 9999;
        string IP = "";
        string content = FileUtils::instance()->getStrFile2string("scadup.cfg");
        if (!content.empty()) {
            IP = FileUtils::instance()->getVariable(content, "IP");
        }
        if (IP.empty()) {
            IP = "192.168.0.6";
            cout << "IP is null when parse 'scadup.cfg', default: " << IP << endl;
        }
        if (method >= CLIENT) {
            scadup.Initialize(IP.c_str(), PORT);
        } else {
            scadup.Initialize(PORT);
        }
        cout << argv[0] << ": run as [" << method << "](" << Scadup::G_MethodValue[method] << ")";
        if (method != SERVER && method != BROKER)
            cout << " to " << IP;
        cout << endl;
        string topic = "topic";
        if (argc > 2) {
            topic = string(argv[2]);
        }
        string param = "a123+/";
        switch (method) {
        case CLIENT:
            scadup.Connect();
            break;
        case SERVER:
            scadup.Start();
            break;
        case BROKER:
            scadup.Broker();
            break;
        case SUBSCRIBE:
            scadup.Subscriber(topic);
            break;
        case PUBLISH:
            if (argc > 4 && string(argv[3]) == "-f") {
                param = argv[4];
                scadup.Publisher(topic, FileUtils::instance()->GetBinFile2String(param));
            } else {
                if (argc == 4) {
                    param = string(argv[3]);
                }
                scadup.Publisher(topic, param);
            }
            break;
        default:
            cout << "method [" << method << "] not implements." << endl;
            break;
        }
        scadup.exit();
#ifdef _USE_FORK_PROCESS_
    } else if (child > 0) {
        cout << "child process " << child << " started" << endl;
    } else {
        cout << "Scadup fork process failed!" << endl;
    }
#endif
}
