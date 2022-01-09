#include "scadop.h"

#ifndef _WIN32
#include <unistd.h>
#endif
#include <iostream>

using namespace std;

int main(int argc, char* argv[]) {
    KaiMethods method = SERVER;
    if (argc > 1) {
        string argv1 = string(argv[1]);
        method = (argv1 == "-C" ? CLIENT :
            (argv1 == "-S" ? SUBSCRIBE :
                (argv1 == "-P" ? PUBLISH :
                    (argv1 == "-B" ? BROKER : SERVER))));
    }
#ifndef _WIN32
    pid_t child = fork();
    if (child == 0) {
#endif
        KaiSocket kai;
        unsigned short PORT = 9999;
        const char* IP = "127.0.0.1";
        if (method >= CLIENT) {
            kai.Initialize(IP, PORT);
        } else {
            kai.Initialize(PORT);
        }
        cout << argv[0] << ": run as [" << method << "](" << KaiSocket::G_KaiMethod[method] << ")" << endl;
        string topic = "topic";
        if (argc > 2) {
            topic = string(argv[2]);
        }
        string payload = "a123+/";
        switch (method) {
        case CLIENT:
            kai.Connect();
            break;
        case SERVER:
            kai.Start();
            break;
        case BROKER:
            kai.Broker();
            break;
        case SUBSCRIBE:
            kai.Subscriber(topic);
            break;
        case PUBLISH:
            if (argc > 3) {
                payload = string(argv[3]);
            }
            kai.Publisher(topic, payload);
        default:
            break;
        }
#ifndef _WIN32
    } else if (child > 0) {
        cout << "child process " << child << " started" << endl;
    } else {
        cout << "KaiSocket fork process failed!" << endl;
    }
#endif
}
