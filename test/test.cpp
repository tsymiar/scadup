#include <scadup.h>
#include <logging.h>
#include <fileutil.h>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace std;
using namespace Scadup;

extern const char* GET_FLAG(Scadup::G_ScaFlag x);

static void usage()
{
    cout << "Usage:" << endl
        << "1                         -- run as broker" << endl
        << "2 [topic] [dst]           -- run as subscriber (dst: one-to-one id, default: local IP)" << endl
        << "3 [topic] [payload] [dst] -- run as publisher messaging to broker (dst empty = broadcast)" << endl
        << "3 [topic] [-f [filename]] -- run as publisher send file content" << endl;
    exit(0);
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
    }
    string IP = "";
    unsigned short PORT = 0;
    string content = FileUtils::instance()->getStrFile2string("scadup.cfg");
    if (!content.empty()) {
        IP = FileUtils::instance()->getVariable(content, "IP");
        PORT = atoi(FileUtils::instance()->getVariable(content, "PORT").c_str());
    }
    if (IP.empty()) {
        IP = "127.0.0.1";
        cout << "IP is null when parse 'scadup.cfg', set default IP: " << IP << endl;
    }
    if (PORT == 0) {
        PORT = 9999;
        cout << "PORT is null when parse 'scadup.cfg', set default PORT: " << PORT << endl;
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
    int state = 0;
    switch (flag) {
    case BROKER:
        state = broker.setup(PORT);
        if (state == 0)
            state = broker.broker();
        break;
    case SUBSCRIBER:
        state = subscriber.setup(IP.c_str(), PORT);
        if (state == 0) {
            uint64_t dst = 0;
            if (argc > 3) {
                dst = strtoull(argv[3], NULL, 0);
            }
            cout << "subscriber ssid=0x" << hex << subscriber.ssid()
                << ", one-to-one id=0x" << subscriber.dstId() << dec << endl;
            state = static_cast<int>(subscriber.subscribe(topic, nullptr, dst));
        }
        break;
    case PUBLISHER:
        state = publisher.setup(IP.c_str(), PORT);
        if (state < 0) break;
        if (argc > 4 && string(argv[3]) == "-f") {
            message = argv[4];
            state = publisher.publish(topic, FileUtils::instance()->GetFileStringContent(message));
        } else {
            if (argc >= 4) {
                message = string(argv[3]);
            }
            uint64_t dst = 0;
            if (argc > 4) {
                dst = strtoull(argv[4], NULL, 0);
            }
            state = publisher.publish(topic, message, dst);
        }
        if (state > 0 && publisher.delivered() >= 0) {
            cout << "delivered to " << publisher.delivered() << " subscriber(s)" << endl;
        }
        break;
    default:
        cout << "flag [" << flag << "] not implements." << endl;
        break;
    }
    cout << argv[0] << ": " << state << endl;
}
