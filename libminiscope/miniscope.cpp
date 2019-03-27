/*
 * Copyright (C) 2019 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the license, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this software.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "miniscope.h"

#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <boost/circular_buffer.hpp>
#include <boost/format.hpp>

#include "definitions.h"
#include "videowriter.h"

#pragma GCC diagnostic ignored "-Wpadded"
class MiniScopeData
{
public:
    MiniScopeData()
        : thread(nullptr),
          scopeCamId(0),
          connected(false),
          running(false),
          recording(false),
          failed(false),
          checkRecTrigger(false),
          droppedFramesCount(0),
          useColor(false)
    {
        frameRing = boost::circular_buffer<cv::Mat>(64);
        videoCodec = VideoCodec::VP9;
        videoContainer = VideoContainer::Matroska;

        showRed = true;
        showGreen = true;
        showBlue = true;

        minFluorDisplay = 0;
        maxFluorDisplay = 255;
    }

    std::thread *thread;
    std::mutex mutex;

    cv::VideoCapture cam;
    int scopeCamId;

    int exposure;
    int gain;
    int excitation;
    uint fps;
    bool excitationX10;
    std::string videoFname;

    double minFluor;
    double maxFluor;
    int minFluorDisplay;
    int maxFluorDisplay;

    bool connected;
    std::atomic_bool running;
    std::atomic_bool recording;
    std::atomic_bool failed;
    std::atomic_bool checkRecTrigger;

    std::chrono::time_point<std::chrono::system_clock> recordStart;

    std::atomic<size_t> droppedFramesCount;
    std::atomic_uint currentFPS;

    boost::circular_buffer<cv::Mat> frameRing;

    std::function<void (std::string)> onMessageCallback;

    bool useColor;
    bool showRed;
    bool showGreen;
    bool showBlue;

    VideoCodec videoCodec;
    VideoContainer videoContainer;
    bool recordLossless;
};
#pragma GCC diagnostic pop


MiniScope::MiniScope()
    : d(new MiniScopeData())
{
    d->exposure = 100;
    d->gain = 32;
    d->excitation = 1;
    d->excitationX10 = false;
    d->fps = 20;
}

MiniScope::~MiniScope()
{
    finishCaptureThread();
    setExcitation(0);
    disconnect();
}

void MiniScope::startCaptureThread()
{
    finishCaptureThread();
    d->running = true;
    d->thread = new std::thread(captureThread, this);
}

void MiniScope::finishCaptureThread()
{
    if (d->thread != nullptr) {
        d->running = false;
        d->thread->join();
        delete d->thread;
        d->thread = nullptr;
    }
}

void MiniScope::emitMessage(const std::string &msg)
{
    if (!d->onMessageCallback) {
        std::cout << msg << std::endl;
        return;
    }

    d->mutex.lock();
    d->onMessageCallback(msg);
    d->mutex.unlock();
}

void MiniScope::fail(const std::string &msg)
{
    d->recording = false;
    d->running = false;
    d->failed = true;
    emitMessage(msg);
}

void MiniScope::setScopeCamId(int id)
{
    d->scopeCamId = id;
}

void MiniScope::setExposure(int value)
{
    if (value == 0)
        value = 1;
    if (value > 100)
        value = 100;

    d->exposure = value;
    d->cam.set(CV_CAP_PROP_BRIGHTNESS, static_cast<double>(d->exposure) / 100);
}

int MiniScope::exposure() const
{
    return d->exposure;
}

void MiniScope::setGain(int value)
{
    d->gain = value;
    d->cam.set(CV_CAP_PROP_GAIN, static_cast<double>(d->gain) / 100);
}

int MiniScope::gain() const
{
    return d->gain;
}

void MiniScope::setExcitation(int value)
{
    d->excitation = value;
    setLed(value);
}

int MiniScope::excitation() const
{
    return d->excitation;
}

bool MiniScope::connect()
{
    if (d->connected) {
        std::cerr << "Tried to reconnect already connected camera." << std::endl;
        return false;
    }

    d->cam.open(d->scopeCamId);

    d->cam.set(CV_CAP_PROP_SATURATION, SET_CMOS_SETTINGS); // Initiallizes CMOS sensor (FPS, gain and exposure enabled...)

    // set default values
    setExposure(100);
    setGain(32);
    setExcitation(1);

    d->connected = true;

    //mValueExcitation = 0;
    //mSliderExcitation.SetPos(mValueExcitation);

    setLed(0);

    emitMessage(boost::str(boost::format("Initialized camera %1%") % d->scopeCamId));

    return true;
}

void MiniScope::disconnect()
{
    stop();
    d->cam.release();
    emitMessage(boost::str(boost::format("Disconnected camera %1%") % d->scopeCamId));
}

bool MiniScope::run()
{
    if (!d->connected)
        return false;
    if (d->failed) {
        // try to recover from failed state by reconnecting
        emitMessage("Reconnecting to recover from previous failure.");
        disconnect();
        if (!connect())
            return false;
    }

    startCaptureThread();
    return true;
}

void MiniScope::stop()
{
    d->running = false;
    d->recording = false;
    finishCaptureThread();
}

bool MiniScope::startRecording(const std::string &fname)
{
    if (!d->connected)
        return false;
    if (!d->running) {
        if (!run())
            return false;
    }

    if (!fname.empty())
        d->videoFname = fname;
    d->recordStart = std::chrono::high_resolution_clock::now();
    d->recording = true;

    return true;
}

void MiniScope::stopRecording()
{
    d->recording = false;
}

bool MiniScope::running() const
{
    return d->running;
}

bool MiniScope::recording() const
{
    return d->running && d->recording;
}

void MiniScope::setOnMessage(std::function<void(const std::string&)> callback)
{
    d->onMessageCallback = callback;
}

bool MiniScope::useColor() const
{
    return d->useColor;
}

void MiniScope::setUseColor(bool color)
{
    d->useColor = color;
}

void MiniScope::setVisibleChannels(bool red, bool green, bool blue)
{
    d->showRed = red;
    d->showGreen = green;
    d->showBlue = blue;
}

bool MiniScope::showRedChannel() const
{
    return d->showRed;
}

bool MiniScope::showGreenChannel() const
{
    return d->showGreen;
}

bool MiniScope::showBlueChannel() const
{
    return d->showBlue;
}

cv::Mat MiniScope::currentFrame()
{
    std::lock_guard<std::mutex> lock(d->mutex);
    cv::Mat frame;
    if (d->frameRing.size() == 0)
        return frame;

    frame = d->frameRing.front();
    d->frameRing.pop_front();
    return frame;
}

uint MiniScope::currentFPS() const
{
    return d->currentFPS;
}

size_t MiniScope::droppedFramesCount() const
{
    return d->droppedFramesCount;
}

bool MiniScope::externalRecordTrigger() const
{
    return d->checkRecTrigger;
}

void MiniScope::setExternalRecordTrigger(bool enabled)
{
    d->checkRecTrigger = enabled;
}

std::string MiniScope::videoFilename() const
{
    return d->videoFname;
}

void MiniScope::setVideoFilename(const std::string &fname)
{
    // TODO: Maybe mutex this, to prevent API users from doing the wrong thing
    // and checking the value directly after the recording was started?
    d->videoFname = fname;
}

VideoCodec MiniScope::videoCodec() const
{
    return d->videoCodec;
}

void MiniScope::setVideoCodec(VideoCodec codec)
{
    d->videoCodec = codec;
}

VideoContainer MiniScope::videoContainer() const
{
    return d->videoContainer;
}

void MiniScope::setVideoContainer(VideoContainer container)
{
    d->videoContainer = container;
}

bool MiniScope::recordLossless() const
{
    return d->recordLossless;
}

void MiniScope::setRecordLossless(bool lossless)
{
    d->recordLossless = lossless;
}

void MiniScope::setLed(int value)
{
    // sanitize value
    if (value > 100)
        value = 100;

    // maximum brighness reached at 50% already, so we divide by two to allow smaller stepsize
    double ledPower = static_cast<double>(value) / 2 / 100;
    if (d->connected) {
        d->cam.set(CV_CAP_PROP_HUE, ledPower);
    }
}

void MiniScope::addFrameToBuffer(const cv::Mat &frame)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    d->frameRing.push_back(frame);
}

void MiniScope::captureThread(void* msPtr)
{
    MiniScope *self = static_cast<MiniScope*> (msPtr);

    auto currentTime = std::chrono::high_resolution_clock::now();
    auto previousTime = currentTime;

    std::vector<int> compression_params;
    compression_params.push_back(CV_IMWRITE_PNG_COMPRESSION);
    compression_params.push_back(0);

    cv::Mat droppedFrameImage(cv::Size(752, 480), CV_8UC3);
    droppedFrameImage.setTo(cv::Scalar(255, 0, 0));
    cv::putText(droppedFrameImage,
                "Frame Dropped!",
                cv::Point(24, 240),
                cv::FONT_HERSHEY_COMPLEX,
                1.5,
                cv::Scalar(255,255,255));

    self->d->droppedFramesCount = 0;

    // prepare for recording
    std::unique_ptr<VideoWriter> vwriter(new VideoWriter());
    auto recordFrames = false;

    while (self->d->running) {
        cv::Mat frame;

        // check if we might want to trigger a recording start via external input
        if (self->d->checkRecTrigger) {
            auto temp = static_cast<int>(self->d->cam.get(CV_CAP_PROP_SATURATION));

            std::cout << "GPIO state: " << temp << std::endl;
            if ((temp & TRIG_RECORD_EXT) == TRIG_RECORD_EXT) {
                if (!self->d->recording) {
                    // start recording
                    self->d->recordStart = std::chrono::high_resolution_clock::now();
                    self->d->recording = true;
                }
            } else {
                // stop recording (if one was running)
                self->d->recording = false;
            }
        }

        auto status = self->d->cam.grab();
        if (!status) {
            self->fail("Failed to grab frame.");
            break;
        }

        try {
            status = self->d->cam.retrieve(frame);
        } catch (cv::Exception& e) {
            status = false;
            std::cerr << "Caught OpenCV exception:" << e.what() << std::endl;
        }

        if (!status) {
            // terminate recording
            self->d->recording = false;

            self->d->droppedFramesCount++;
            self->emitMessage("Dropped frame.");
            self->addFrameToBuffer(droppedFrameImage);
            if (self->d->droppedFramesCount > 0) {
                self->emitMessage("Reconnecting Miniscope...");
                self->d->cam.release();
                self->d->cam.open(self->d->scopeCamId);
                self->emitMessage("Miniscope reconnected.");
            }

            if (self->d->droppedFramesCount > 80)
                self->fail("Too many dropped frames. Giving up.");
            continue;
        }

        previousTime = currentTime;
        currentTime = std::chrono::high_resolution_clock::now();
        auto delayTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - previousTime);
        self->d->currentFPS = static_cast<uint>(1 / (delayTime.count() / static_cast<double>(1000)));

        // check if we are too slow, resend settings in case we are
        // NOTE: This behaviour was copied from the original Miniscope DAQ software
        // Previously, a second OR conadition was: self->d->currentFPS < self->d->fps / 2.0
        if (self->d->droppedFramesCount > 0) {
            self->emitMessage("Sending settings again.");
            self->d->cam.set(CV_CAP_PROP_BRIGHTNESS, self->d->exposure);
            self->d->cam.set(CV_CAP_PROP_GAIN, self->d->gain);
            self->setLed(self->d->excitation);
            self->d->droppedFramesCount = 0;
        }

        // "frame" is the frame that we record to disk, while the "displayFrame"
        // is the one that we may also record as a video file
        cv::Mat displayFrame;
        frame.copyTo(displayFrame);

        if (self->d->useColor) {
            // we want a colored image
            if (self->d->showRed || self->d->showGreen || self->d->showBlue) {
                cv::Mat bgrChannels[3];
                cv::split(frame,bgrChannels);

                if (!self->d->showBlue)
                    bgrChannels[0] = cv::Mat::zeros(frame.rows, frame.cols, CV_8UC1);
                if (!self->d->showGreen)
                    bgrChannels[1] = cv::Mat::zeros(frame.rows, frame.cols, CV_8UC1);
                if (!self->d->showRed)
                    bgrChannels[2] = cv::Mat::zeros(frame.rows, frame.cols, CV_8UC1);

                cv::merge(bgrChannels, 3, frame);
            }
         } else {
            // grayscale image
            cv::cvtColor(frame, frame, CV_BGR2GRAY); // added to correct green color stream

            cv::minMaxLoc(frame, &self->d->minFluor, &self->d->maxFluor);
            frame.convertTo(displayFrame, CV_8U, 255.0 / (self->d->maxFluorDisplay - self->d->minFluorDisplay), -self->d->minFluorDisplay * 255.0 / (self->d->maxFluorDisplay - self->d->minFluorDisplay));
        }

        // prepare video recording if it was enabled while we were running
        if (self->recording()) {
            if (!vwriter->initialized()) {
                self->emitMessage("Recording enabled.");
                // we want to record, but are not initialized yet
                vwriter->setCodec(self->d->videoCodec);
                vwriter->setContainer(self->d->videoContainer);
                vwriter->setLossless(self->d->recordLossless);

                try {
                    vwriter->initialize(self->d->videoFname,
                                        frame.cols,
                                        frame.rows,
                                        static_cast<int>(self->d->fps),
                                        frame.channels() == 3);
                } catch (cv::Exception& e) {
                    self->fail(boost::str(boost::format("Unable to initialize recording: %1%") % e.what()));
                    break;
                }

                // we are set for recording and initialized the video writer,
                // so we allow recording frames now
                recordFrames = true;
                self->emitMessage("Initialized video recording.");
            }
        } else {
            // we are not recording or stopped recording
            if (recordFrames) {
                // we were recording previously, so finalize the movie and stop adding
                // new frames to the video.
                // Also reset the video writer for a clean start
                vwriter->finalize();
                vwriter.reset(new VideoWriter());
                recordFrames = false;
                self->emitMessage("Recording finalized.");
            }
        }

        // add display frame to ringbuffer, and record the raw
        // frame to disk if we want to record it.
        self->addFrameToBuffer(displayFrame);
        if (recordFrames)
            vwriter->encodeFrame(frame);

        //std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // finalize recording (if there was any still ongoing)
    vwriter->finalize();
}