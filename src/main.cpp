/*
* Copyright (c) 2018 Intel Corporation.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
* LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
* OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
* WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

// std includes
#include <iostream>
#include <stdio.h>
#include <thread>
#include <queue>
#include <map>
#include <atomic>
#include <csignal>
#include <mutex>
#include <syslog.h>

// OpenCV includes
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

// MQTT
#include "mqtt.h"

using namespace std;
using namespace cv;
using namespace dnn;

// OpenCV-related variables
Mat frame, blob, sentBlob;
VideoCapture cap;
int delay = 5;
Net net, sentnet;
bool sentChecked = false;

// application parameters
String model;
String config;
String sentmodel;
String sentconfig;
int backendId;
int targetId;
int rate;
float confidenceFace;
float confidenceMood;

// flag to control background threads
atomic<bool> keepRunning(true);

// flag to handle UNIX signals
static volatile sig_atomic_t sig_caught = 0;

// mqtt parameters
const string topic = "retail/traffic";

// Sentiment contains different kinds of emotions
enum Sentiment {
        Neutral,
        Happy,
        Sad,
        Surprised,
        Anger,
        Unknown
};

// ShoppingInfo contains statistics for the shopping information tracked by this application.
struct ShoppingInfo
{
    int shoppers;
    map<Sentiment, int> sent;
};

// currentInfo contains the latest ShoppingInfo tracked by the application.
ShoppingInfo currentInfo;

queue<Mat> nextImage;
String currentPerf;

mutex m, m1, m2;

const char* keys =
    "{ help  h      | | Print help message. }"
    "{ device d     | 0 | camera device number. }"
    "{ input i      | | Path to input image or video file. Skip this argument to capture frames from a camera.}"
    "{ model m      | | Path to .bin file of model containing face recognizer. }"
    "{ config c     | | Path to .xml file of model containing network configuration. }"
    "{ faceconf fc  | 0.5 | Confidence factor for face detection required. }"
    "{ moodconf mc  | 0.5 | Confidence factor for emotion detection required. }"
    "{ sentmodel sm     | | Path to .bin file of sentiment model. }"
    "{ sentconfig sc    | | Path to a .xml file of sentiment model containing network configuration. }"
    "{ backend b    | 0 | Choose one of computation backends: "
                        "0: automatically (by default), "
                        "1: Halide language (http://halide-lang.org/), "
                        "2: Intel's Deep Learning Inference Engine (https://software.intel.com/openvino-toolkit), "
                        "3: OpenCV implementation }"
    "{ target t     | 0 | Choose one of target computation devices: "
                        "0: CPU target (by default), "
                        "1: OpenCL, "
                        "2: OpenCL fp16 (half-float precision), "
                        "3: VPU }"
    "{ rate r      | 1 | number of seconds between data updates to MQTT server. }";


// nextImageAvailable returns the next image from the queue in a thread-safe way
Mat nextImageAvailable() {
    Mat rtn;
    m.lock();
    if (!nextImage.empty()) {
        rtn = nextImage.front();
        nextImage.pop();
    }
    m.unlock();
    return rtn;
}

// addImage adds an image to the queue in a thread-safe way
void addImage(Mat img) {
    m.lock();
    if (nextImage.empty()) {
        nextImage.push(img);
    }
    m.unlock();
}

// getCurrentInfo returns the most-recent ShoppingInfo for the application.
ShoppingInfo getCurrentInfo() {
    ShoppingInfo rtn;
    m2.lock();
    rtn = currentInfo;
    m2.unlock();
    return rtn;
}

// updateInfo uppdates the current ShoppingInfo for the application to the highest values
// during the current time period.
void updateInfo(ShoppingInfo info) {
    m2.lock();
    if (currentInfo.shoppers < info.shoppers) {
        currentInfo.shoppers = info.shoppers;
    }

    for (pair<Sentiment, int> element : info.sent) {
	Sentiment s = element.first;
	int count = element.second;
	if (currentInfo.sent[s] < info.sent[s]) {
            currentInfo.sent[s] = info.sent[s];
        }
    }
    m2.unlock();
}

// resetInfo resets the current ShoppingInfo for the application.
void resetInfo() {
    m2.lock();
    currentInfo.shoppers = 0;
    for (pair<Sentiment, int> element : currentInfo.sent) {
	Sentiment s = element.first;
        currentInfo.sent[s] = 0;
    }
    m2.unlock();
}

// getCurrentPerf returns a display string with the most current performance stats for the Inference Engine.
string getCurrentPerf() {
    string rtn;
    m1.lock();
    rtn = currentPerf;
    m1.unlock();
    return rtn;
}

// savePerformanceInfo sets the display string with the most current performance stats for the Inference Engine.
void savePerformanceInfo() {
    m1.lock();

    vector<double> faceTimes, sentTimes;
    double freq = getTickFrequency() / 1000;
    double t = net.getPerfProfile(faceTimes) / freq;
    double t2;
    if (sentChecked) {
        t2 = sentnet.getPerfProfile(sentTimes) / freq;
    }
    string label = format("Face inference time: %.2f ms, Sentiment inference time: %.2f ms", t, t2);

    currentPerf = label;

    m1.unlock();
}

// publish MQTT message with a JSON payload
void publishMQTTMessage(const string& topic, const ShoppingInfo& info)
{
    ostringstream s;
    s << "{\"shoppers\": \"" << info.shoppers << "\",";
    s << "\"neutral\": \"" << info.sent.at(Neutral) << "\"";
    s << "\"happy\": \"" << info.sent.at(Happy) << "\"";
    s << "\"sad\": \"" << info.sent.at(Sad) << "\"";
    s << "\"surprised\": \"" << info.sent.at(Surprised) << "\"";
    s << "\"anger\": \"" << info.sent.at(Anger) << "\"}";
    string payload = s.str();

    mqtt_publish(topic, payload);

    string msg = "MQTT message published to topic: " + topic;
    syslog(LOG_INFO, "%s", msg.c_str());
    syslog(LOG_INFO, "%s", payload.c_str());
}

// message handler for the MQTT subscription for the any desired control channel topic
int handleMQTTControlMessages(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    string topic = topicName;
    string msg = "MQTT message received: " + topic;
    syslog(LOG_INFO, "%s", msg.c_str());

    return 1;
}

// Function called by worker thread to process the next available video frame.
void frameRunner() {
    while (keepRunning.load()) {
        Mat next = nextImageAvailable();
        if (!next.empty()) {
            // convert to 4d vector as required by model, and set as input
            blobFromImage(next, blob, 1.0, Size(672, 384));
            net.setInput(blob);
            Mat prob = net.forward();

            // get faces
            vector<Rect> faces;
            float* data = (float*)prob.data;
            for (size_t i = 0; i < prob.total(); i += 7)
            {
                float confidence = data[i + 2];
                if (confidence > confidenceFace)
                {
                    int left = (int)(data[i + 3] * frame.cols);
                    int top = (int)(data[i + 4] * frame.rows);
                    int right = (int)(data[i + 5] * frame.cols);
                    int bottom = (int)(data[i + 6] * frame.rows);
                    int width = right - left + 1;
                    int height = bottom - top + 1;

                    faces.push_back(Rect(left, top, width, height));
                }
            }

            //int detSentiment;
            map<Sentiment, int> sent = {
                    {Neutral, 0},
                    {Happy, 0},
                    {Sad, 0},
                    {Surprised, 0},
                    {Anger, 0},
                    {Unknown, 0}
            };
            // detect sentiment
            for(auto const& r: faces) {
                // make sure the face rect is completely inside the main Mat
                if ((r & Rect(0, 0, next.cols, next.rows)) != r) {
                    continue;
                }

                // Read detected face
                Mat face = next(r);

                // convert to 4d vector, and propagate through sentiment Neural Network
                blobFromImage(face, sentBlob, 1.0, Size(64, 64));
                sentnet.setInput(sentBlob);
                Mat prob = sentnet.forward();
                sentChecked = true;

                // flatten the result from [1, 5, 1, 1] to [1, 5]
                Mat flat = prob.reshape(1, 5);
                // Find the max in returned list of sentiments
                Point maxLoc;
                double confidence;
                minMaxLoc(flat, 0, &confidence, 0, &maxLoc);
                Sentiment s;
                if (confidence > static_cast<double>(confidenceMood)) {
                    s = static_cast<Sentiment>(maxLoc.y);
                } else {
                    s = Unknown;
                }
                sent[s] = sent.at(s) + 1;
            }

            // retail data
            ShoppingInfo info;
            info.shoppers = faces.size();
            info.sent = sent;
            updateInfo(info);

            savePerformanceInfo();
        }
    }

    cout << "Video processing thread stopped" << endl;
}

// publishInfo publishes current shoppingInfo to MQTT topic queue and then resets it
void publishAndResetCurrentInfo()
{
    m2.lock();
    ShoppingInfo rtn = currentInfo;
    publishMQTTMessage(topic, rtn);
    currentInfo.shoppers = 0;
    for (pair<Sentiment, int> element : currentInfo.sent) {
	Sentiment s = element.first;
        currentInfo.sent[s] = 0;
    }
    m2.unlock();
}

// Function called by worker thread to handle MQTT updates. Pauses for rate second(s) between updates.
void messageRunner() {
    while (keepRunning.load()) {
        publishAndResetCurrentInfo();
        this_thread::sleep_for(chrono::seconds(rate));
    }

    cout << "MQTT sender thread stopped" << endl;
}

// signal handler for the main thread
void handle_sigterm(int signum)
{
    /* we only handle SIGTERM and SIGKILL here */
    if (signum == SIGTERM) {
        cout << "Interrupt signal (" << signum << ") received" << endl;
        sig_caught = 1;
    }
}

int main(int argc, char** argv)
{
    // parse command parameters
    CommandLineParser parser(argc, argv, keys);
    parser.about("Use this script to using OpenVINO.");
    if (argc == 1 || parser.has("help"))
    {
        parser.printMessage();
        return 0;
    }

    model = parser.get<String>("model");
    config = parser.get<String>("config");
    backendId = parser.get<int>("backend");
    targetId = parser.get<int>("target");
    rate = parser.get<int>("rate");
    confidenceFace = parser.get<float>("faceconf");
    confidenceMood = parser.get<float>("moodconf");

    sentmodel = parser.get<String>("sentmodel");
    sentconfig = parser.get<String>("sentconfig");

    // connect MQTT messaging
    int result = mqtt_start(handleMQTTControlMessages);
    if (result == 0) {
        syslog(LOG_INFO, "MQTT started.");
    } else {
        syslog(LOG_INFO, "MQTT NOT started: have you set the ENV varables?");
    }

    mqtt_connect();

    // open face model
    net = readNet(model, config);
    net.setPreferableBackend(backendId);
    net.setPreferableTarget(targetId);

    // open pose model
    sentnet = readNet(sentmodel, sentconfig);
    sentnet.setPreferableBackend(backendId);
    sentnet.setPreferableTarget(targetId);

    // open video capture source
    if (parser.has("input")) {
        cap.open(parser.get<String>("input"));

        // also adjust delay so video playback matches the number of FPS in the file
        double fps = cap.get(CAP_PROP_FPS);
        delay = 1000/fps;
    }
    else
        cap.open(parser.get<int>("device"));

    if (!cap.isOpened()) {
        cerr << "ERROR! Unable to open video source\n";
        return -1;
    }

    // initialize shopping info
    currentInfo.shoppers = 0;
    currentInfo.sent = {
            {Neutral, 0},
            {Happy, 0},
            {Sad, 0},
            {Surprised, 0},
            {Anger, 0},
            {Unknown, 0}
    };

    // register signal handler
    signal(SIGTERM, handle_sigterm);

    // start worker threads
    thread t1(frameRunner);
    thread t2(messageRunner);

    // read video input data
    for (;;) {
        cap.read(frame);

        if (frame.empty()) {
            keepRunning = false;
            cerr << "ERROR! blank frame grabbed\n";
            break;
        }

        addImage(frame);

        string label = getCurrentPerf();
        putText(frame, label, Point(0, 15), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 0, 0));

        ShoppingInfo info = getCurrentInfo();
        label = format("Shoppers: %d, Neutral: %d, Happy: %d, Sad: %d, Surprised: %d, Anger: %d, Unknown: %d",
                        info.shoppers, info.sent[Neutral], info.sent[Happy], info.sent[Sad],
                        info.sent[Surprised], info.sent[Anger], info.sent[Unknown]);
        putText(frame, label, Point(0, 40), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 0, 0));

        imshow("Shopper Sentiment Monitor", frame);

        if (waitKey(delay) >= 0 || sig_caught) {
            cout << "Attempting to stop background threads" << endl;
            keepRunning = false;
            break;
        }
    }

    // wait for the threads to finish
    t1.join();
    t2.join();

    // disconnect MQTT messaging
    mqtt_disconnect();
    mqtt_close();

    return 0;
}
