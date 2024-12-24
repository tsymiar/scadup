#include "scadup/Scadup.h"
#include "utils/FileUtils.h"
#include <iostream>

using namespace std;
using namespace Scadup;

extern const char* GET_FLAG(Scadup::G_ScaFlag x);

static void usage()
{
    cout << "Usage:" << endl
        << "1         -- run as broker" << endl
        << "2 [topic] -- run as subscriber" << endl
        << "3 [topic] [payload] -- run as publisher messaging to broker" << endl
        << "3 [topic] [-f [filename]] -- run as publisher send file content" << endl;
}

int main(int argc, char* argv[])
{
    G_ScaFlag flag = NONE;
    if (argc > 1) {
        string argv1 = string(argv[1]);
        flag =
            (argv1 == "1" ? BROKER :
                (argv1 == "2" ? SUBSCRIBER :
                    (argv1 == "3" ? PUBLISHER : BROKER)));
    } else {
        usage();
        return 0;
    }
    unsigned short PORT = 9999;
    string IP = "";
    string content = FileUtils::instance()->getStrFile2string("scadup.cfg");
    if (!content.empty()) {
        IP = FileUtils::instance()->getVariable(content, "IP");
    }
    if (IP.empty()) {
        IP = "127.0.0.1";
        cout << "IP is null when parse 'scadup.cfg', set default IP: " << IP << endl;
    }
    cout << argv[0] << ": " << GET_FLAG(flag) << " test start." << endl;
    uint32_t topic = 0x1234;
    if (argc > 2) {
        topic = strtol(argv[2], NULL, 16);
    }
    string message = "a123+/";
    Broker broker;
    Publisher publisher;
    Subscriber subscriber;
    switch (flag) {
    case BROKER:
        broker.setup();
        broker.broker();
        break;
    case SUBSCRIBER:
        subscriber.setup(IP.c_str(), PORT);
        subscriber.subscribe(topic);
        break;
    case PUBLISHER:
        publisher.setup(IP.c_str(), PORT);
        if (argc > 4 && string(argv[3]) == "-f") {
            message = argv[4];
            publisher.publish(topic, FileUtils::instance()->GetFileStringContent(message));
        } else {
            if (argc == 4) {
                message = string(argv[3]);
            }
            publisher.publish(topic, message);
        }
        break;
    default:
        cout << "flag [" << flag << "] not implements." << endl;
        break;
    }
}
