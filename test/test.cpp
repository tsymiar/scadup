#include "Scadup.h"

#ifndef _WIN32
#include <unistd.h>
// #define _USE_FORK_PROCESS_
#endif
#include <iostream>

using namespace std;

int main(int argc, char* argv[]) {
    G_MethodEnum method = SERVER;
    if (argc > 1) {
        string argv1 = string(argv[1]);
        method = (argv1 == "-C" ? CLIENT :
            (argv1 == "-S" ? SUBSCRIBE :
                (argv1 == "-P" ? PUBLISH :
                    (argv1 == "-B" ? BROKER : SERVER))));
    }
#ifdef _USE_FORK_PROCESS_
    pid_t child = fork();
    if (child == 0) {
#endif
        Scadup scadup;
        unsigned short PORT = 9999;
        const char* IP = "81.68.170.12";
        if (method >= CLIENT) {
            scadup.Initialize(IP, PORT);
        } else {
            scadup.Initialize(PORT);
        }
        cout << argv[0] << ": run as [" << method << "](" << Scadup::G_MethodValue[method] << ")" << endl;
        string topic = "topic";
        if (argc > 2) {
            topic = string(argv[2]);
        }
        string payload = "a123+/";
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
            if (argc > 3) {
                payload = string(argv[3]);
            }
            scadup.Publisher(topic, payload);
        default:
            break;
        }
#ifdef _USE_FORK_PROCESS_
    } else if (child > 0) {
        cout << "child process " << child << " started" << endl;
    } else {
        cout << "Scadup fork process failed!" << endl;
    }
#endif
}
