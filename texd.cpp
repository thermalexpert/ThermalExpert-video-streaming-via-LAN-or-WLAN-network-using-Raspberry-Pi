/* Copyright (c) 2020 Voltagezone Electronics e.U. <info@thermalexpert.eu> */
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "i3system_TE.h"
#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#ifdef OPENCV2
#include "opencv2/contrib/contrib.hpp"
#endif
#include <libconfig.h++>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define VERSION_NUMBER "1.2.0"

using namespace cv;
using namespace std;
using namespace i3;
using namespace libconfig;

void applyOverlay(Mat &img, int width, int height);
void receiveImage(int socket);
void *mainThread(void *ptr);
void deviceChangedCallback(TE_STATE _in);
void stopFfmpeg();
int readConfig();
void signalHandler(int signum);
void calibrateToFile(int signum);

static TE_B *device_TE_B = NULL;
static bool calibration_ready = false;
static int camera_model = -1;
static pid_t ffmpeg_pid = -1;

hotplug_callback_func gCallback;
bool stop_receiver = false;
bool stop_signaled = false;

static string setting_calibration_directory_path;
static string setting_image_overlay;
static int setting_internal_tcp_port;
static int setting_ffmpeg_server_tcp_port;
static float setting_emissivity;
static int temp_offset;
static int setting_rotation;
static int img_header_y =
    40; // Number of lines the image is extended vertically

void signalHandler(int signum) {
  stop_signaled = true;
  stop_receiver = true;
  stopFfmpeg();
  cout << "Stopped by signal (PID:" << getpid() << ")" << endl;
  if (signum == SIGALRM) {
    cerr << "Timeout! Please reconnect device!" << endl;
    kill(getpid(), SIGKILL);
    exit(-1);
  }
  exit(0);
}

void calibrateToFile(int signum) {
  if (!calibration_ready) {
    cerr << "Device not ready for calibration!" << endl;
    return;
  }

  if (device_TE_B->ShutterCalibrationOn() != 1) {
    cerr << "Failed to calibrate!" << endl;
  } else {
    if (device_TE_B->SaveCalibration(
            setting_calibration_directory_path.c_str()))
      cout << "Successfully saved calibration data to "
           << setting_calibration_directory_path << endl;
    else
      cerr << "Saving calibration data to "
           << setting_calibration_directory_path << " failed!" << endl;
  }
}

int readConfig() {
  Config cfg;

  try {
    cfg.readFile("/etc/texd/texd.conf");
  } catch (const FileIOException &fioex) {
    std::cerr << "I/O error while reading configuration file." << std::endl;
    return (EXIT_FAILURE);
  } catch (const ParseException &pex) {
    std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
              << " - " << pex.getError() << std::endl;
    return (EXIT_FAILURE);
  }

  try {
    setting_calibration_directory_path =
        cfg.lookup("calibration_directory_path").c_str();
    setting_image_overlay = cfg.lookup("image_overlay").c_str();
    setting_internal_tcp_port =
        std::stoi(cfg.lookup("internal_tcp_port").c_str());
    setting_ffmpeg_server_tcp_port =
        std::stoi(cfg.lookup("ffmpeg_server_tcp_port").c_str());
    setting_emissivity = std::stof(cfg.lookup("emissivity").c_str());
    setting_rotation = std::stoi(cfg.lookup("rotation").c_str());
    temp_offset = std::stoi(cfg.lookup("temp_offset").c_str());

    cout << "Settings from configuration file:" << endl;
    cout << "Calibration directory: " << setting_calibration_directory_path
         << endl;
    cout << "Image overlay mode: " << setting_image_overlay << endl;
    cout << "Internal TCP port: " << setting_internal_tcp_port << endl;
    cout << "FFMPEG server TCP port: " << setting_ffmpeg_server_tcp_port
         << endl;
    cout << "Emissivity: " << setting_emissivity << endl;
    cout << "Rotation: " << setting_rotation << endl;
    cout << "Temperature offset: " << temp_offset << endl;
    cout << endl;

    struct stat st;
    stat(setting_calibration_directory_path.c_str(), &st);
    bool isdir = S_ISDIR(st.st_mode);

    if (!isdir) {
      cerr << "Calibration directory path is invalid: "
           << setting_calibration_directory_path << endl;
      return (EXIT_FAILURE);
    }

    if (!(setting_image_overlay == "on" || setting_image_overlay == "off" ||
          setting_image_overlay == "temperature" ||
          setting_image_overlay == "marker")) {
      cerr << "Image overlay setting is invalid (must be "
              "on|off|temperature|marker): "
           << setting_image_overlay << endl;
      return (EXIT_FAILURE);
    }

    if (setting_image_overlay == "off" || setting_image_overlay == "marker")
      img_header_y = 0;
    else
      img_header_y = 40;

    if (setting_internal_tcp_port > 0xffff) {
      cerr << "Internal TCP port setting is invalid: "
           << setting_internal_tcp_port << endl;
      return (EXIT_FAILURE);
    }

    if (setting_emissivity > 100.0 || setting_emissivity < 1) {
      cerr << "Emissivity setting is invalid (must be 1.0-100.0): "
           << setting_emissivity << endl;
      return (EXIT_FAILURE);
    } else
      setting_emissivity = setting_emissivity / 10.f;

    if (temp_offset > 20 || temp_offset < -20) {
      cerr << "Temperature offset out of range! (must be 20 <-> -20): "
           << temp_offset << endl;
      return (EXIT_FAILURE);
    }

    if (setting_rotation == 90)
      setting_rotation = RotateFlags::ROTATE_90_CLOCKWISE;
    else if (setting_rotation == 180)
      setting_rotation = RotateFlags::ROTATE_180;
    else if (setting_rotation == 270)
      setting_rotation = RotateFlags::ROTATE_90_COUNTERCLOCKWISE;
    else if (setting_rotation == 0)
      setting_rotation = -1;
    else {
      cerr << "Rotation setting is invalid: " << setting_rotation << endl;
      return (EXIT_FAILURE);
    }

  } catch (const SettingNotFoundException &nfex) {
    cerr << endl;
    cerr << "Fatal error during configuration file parsing!" << endl;
    return (EXIT_FAILURE);
  } catch (const std::invalid_argument &ia) {
    cerr << endl;
    cerr << "Fatal error during configuration file parsing!" << endl;
    std::cerr << "Invalid argument: " << ia.what() << '\n';
    return (EXIT_FAILURE);
  } catch (const std::out_of_range &oor) {
    cerr << endl;
    cerr << "Fatal error during configuration file parsing!" << endl;
    std::cerr << "Out of Range error: " << oor.what() << '\n';
    return (EXIT_FAILURE);
  }

  return 0;
}

/* Receive image data and send to ffmpeg */
void receiveImage(int socket) {

  if (device_TE_B) {
    // 	auto t1 = std::chrono::high_resolution_clock::now();
    // 	auto t2 = std::chrono::high_resolution_clock::now();
    // 	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
    // t2 - t1 ).count(); 	std::cout << "Time: " << duration << "us" <<
    // endl;

    int width = device_TE_B->GetImageWidth();
    int height = device_TE_B->GetImageHeight();

    unsigned short *buf = new unsigned short[width * height];

    int status = device_TE_B->RecvImage(buf); // 5ms M1 13ms Q1

    if (status == 1) {
      Mat img(height, width, CV_16U, buf);

      if (socket >= 0) {
        img.convertTo(img, CV_8UC1, 1. / 256.); // Convert to RGB
        applyColorMap(img, img, COLORMAP_JET);  // Apply heatmap
        applyOverlay(img, width, height);       // 5ms M1 8ms Q1
        Mat flat = img.reshape(
            1, img.total() * img.channels()); // Convert matrix to vector
        vector<uchar> vec = img.isContinuous()
                                ? flat
                                : flat.clone(); // Convert matrix to vector
        uchar *p_vector = &vec[0]; // Convert vector to array pointer

        int status = send(socket, p_vector, vec.size(), MSG_NOSIGNAL);
        if (status < 0) {
          cout << "Send failed! (client disconnect)" << endl;
          stop_receiver = true;
          stopFfmpeg();
        }
      }

      // imwrite(filename, img ); // Debug to file
      // imshow("Image", img); // Debug to window
      // waitKey(1);
    } else {
      // cerr << "Received image incomplete and discarded (" << status << ")" <<
      // endl;
      // NOTE: HW frequency is limited to 9 Hz, if we poll faster we get
      // acceptable losses
    }
    delete[] buf;

  } else {
    cerr << "Hard fault!" << endl;
    stop_receiver = true;
    stopFfmpeg();
    exit(-1);
  }
}

/* Calc min/max temperature and add overlay */
void applyOverlay(Mat &img, int width, int height) {

  if (setting_image_overlay == "off") {
    if (setting_rotation != -1)
      rotate(img, img, setting_rotation);
    return;
  }

  int size = width * height;
  unsigned short *buf = new unsigned short[size];
  float *temp = new float[size];
  float minTemp = FLT_MAX, maxTemp = -FLT_MAX;
  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  device_TE_B->SetEmissivity(setting_emissivity);
  device_TE_B->CalcEntireTemp(temp);

  for (int y = 0, pos = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x, ++pos) {
      float t = temp[pos];
      if (maxTemp < t) {
        maxTemp = t;
        maxX = x;
        maxY = y;
      }

      if (minTemp > t) {
        minTemp = t;
        minX = x;
        minY = y;
      }
    }
  }

  if (setting_image_overlay == "marker" || setting_image_overlay == "on") {
    line(img, Point(minX - 5, minY), Point(minX + 5, minY),
         Scalar(255, 255, 255), 3);
    line(img, Point(minX, minY - 5), Point(minX, minY + 5),
         Scalar(255, 255, 255), 3);

    line(img, Point(maxX - 5, maxY), Point(maxX + 5, maxY), Scalar(0, 0, 0), 3);
    line(img, Point(maxX, maxY - 5), Point(maxX, maxY + 5), Scalar(0, 0, 0), 3);
  }

  if (setting_rotation != -1)
    rotate(img, img, setting_rotation); // Must be done after marker!

  if (setting_image_overlay == "temperature" || setting_image_overlay == "on") {

    char str[200];
    char str2[200];
    maxTemp += temp_offset;
    minTemp += temp_offset;

    if (camera_model == I3_TE_Q1) {
      copyMakeBorder(img, img, img_header_y / 2, 0, 0, 0, BORDER_CONSTANT,
                     Scalar(128, 128, 128));
      sprintf(str, "max: %.2f'C", maxTemp);
      sprintf(str2, "min: %.2f'C", minTemp);

      putText(img, str, Point2f(10, 15), FONT_HERSHEY_PLAIN, 1,
              Scalar(0, 0, 0));

      putText(img, str2, Point2f(170, 15), FONT_HERSHEY_PLAIN, 1,
              Scalar(255, 255, 255));
    } else {
      copyMakeBorder(img, img, img_header_y, 0, 0, 0, BORDER_CONSTANT,
                     Scalar(128, 128, 128));
      sprintf(str, "%.2f'C max", maxTemp);
      sprintf(str2, "%.2f'C min", minTemp);

      putText(img, str, Point2f(10, 15), FONT_HERSHEY_PLAIN, 1,
              Scalar(0, 0, 0));
      putText(img, str2, Point2f(10, 15 * 2 + 4), FONT_HERSHEY_PLAIN, 1,
              Scalar(255, 255, 255));
    }
  }

  delete[] buf;
  delete[] temp;
}

/* Stops ffmpeg child process */
void stopFfmpeg() {
  if (ffmpeg_pid != -1) {
    kill(ffmpeg_pid, SIGTERM);
    ffmpeg_pid = -1;
  }
}

/* Starts ffmpeg child process */
void startFfmpeg() {
  if (ffmpeg_pid == -1) {
    ffmpeg_pid = fork(); // create child process
    if (ffmpeg_pid == 0) // if child process
    {
      // This code is executed in the child process
      std::string video_size;
      if (camera_model == I3_TE_Q1) {
        if (setting_rotation == RotateFlags::ROTATE_90_CLOCKWISE ||
            setting_rotation == RotateFlags::ROTATE_90_COUNTERCLOCKWISE) {
          video_size = "288x";
          video_size.append(to_string(384 + img_header_y / 2));
        } else {
          video_size = "384x";
          video_size.append(to_string(288 + img_header_y / 2));
        }
      } else if (camera_model == I3_TE_M1) {
        if (setting_rotation == RotateFlags::ROTATE_90_CLOCKWISE ||
            setting_rotation == RotateFlags::ROTATE_90_COUNTERCLOCKWISE) {
          video_size = "180x";
          video_size.append(to_string(240 + img_header_y));
        } else {
          video_size = "240x";
          video_size.append(to_string(180 + img_header_y));
        }
      } else {
        cerr << "Invalid model!" << endl;
        return;
      }

      string internal_tcp = "tcp://127.0.0.1:";
      internal_tcp.append(std::to_string(setting_internal_tcp_port));
      string ffmpeg_server_tcp = "http://127.0.0.1:";
      ffmpeg_server_tcp.append(std::to_string(setting_ffmpeg_server_tcp_port));
      ffmpeg_server_tcp.append("/camera.ffm");

      cout << "ffmpeg child: Waiting for server to start... " << endl;
      usleep(5 * 1000 * 1000); // Wait X seconds to avoid race condition
      cout << "ffmpeg child: Trying to connect" << endl;

      execl("/usr/bin/ffmpeg_3.4.7", "ffmpegstreamer", "-hide_banner",
            "-loglevel", "warning", "-re", "-f", "rawvideo", "-pixel_format",
            "bgr24", "-framerate", "24", "-fflags", "nobuffer", "-flags",
            "low_delay", "-video_size", video_size.c_str(), "-i",
            internal_tcp.c_str(), "-vcodec", "copy", "-f", "ffm",
            ffmpeg_server_tcp.c_str(), NULL);

      cerr << "ffmpeg child: STARTING FFMPEG FAILED (NOT INSTALLED?)!" << endl;
    }

  } else
    cerr << "Child already started " << ffmpeg_pid << endl;
}

void *mainThread(void *ptr) {

  int server_fd, new_socket, valread;
  struct sockaddr_in address;
  int opt = 1;
  int addrlen = sizeof(address);

  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    cerr << "Failed setting up socket!" << endl;
    exit(EXIT_FAILURE);
  }

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
                 sizeof(opt))) {
    cerr << "Failed setting up socket!" << endl;
    exit(EXIT_FAILURE);
  }

  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(setting_internal_tcp_port);

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    cerr << "Failed binding socket!" << endl;
    exit(EXIT_FAILURE);
  }
  if (listen(server_fd, 1) < 0) {
    cerr << "Failed listen on socket!" << endl;
    exit(EXIT_FAILURE);
  }

  string file_name = "";
  if (camera_model == I3_TE_Q1)
    file_name = "q1.dat";
  else if (camera_model == I3_TE_M1)
    file_name = "m1.dat";

  if (device_TE_B->LoadCalibration(
          setting_calibration_directory_path.append("/")
              .append(file_name)
              .c_str()) == 1)
    cout << "Successfully loaded calibration data from file ("
         << setting_calibration_directory_path << ")" << endl;
  else
    cerr << "Failed loading calibration data! ("
         << setting_calibration_directory_path << ")" << endl;

  do {
    stop_receiver = false;
    if (stop_signaled)
      break;
    startFfmpeg();
    cout << "Waiting for client to connect..." << endl;

    if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
                             (socklen_t *)&addrlen)) < 0) {
      cerr << "Waiting for client failed!" << endl;
      stopFfmpeg();
      return NULL;
    }

    cout << "Client connected: " << new_socket << endl;

    while (!stop_receiver) {
      receiveImage(new_socket);
      usleep(50 * 1000); // Poll every X ms
    }
  } while (1);

  stopFfmpeg();
  return NULL;
}

void deviceChangedCallback(TE_STATE _in) {
  //     if(_in.nUsbState == TE_ARRIVAL){
  //         cout << "Device Attached" << endl;
  //     }

  if (_in.nUsbState == TE_REMOVAL) {
    cout << "Device detached and program stopped!" << endl;
    stop_signaled = true;
    stop_receiver = true;
    stopFfmpeg();
    exit(-1);
  }
}

int main() {
  cout << "texd " << VERSION_NUMBER << endl;

  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);
  signal(SIGALRM, signalHandler);
  signal(SIGPIPE, signalHandler);
  signal(SIGUSR1, calibrateToFile);

  gCallback = &deviceChangedCallback;
  SetHotplugCallback(gCallback);

  if (readConfig() != 0)
    exit(-1);

  TEScanData devs[MAX_USB_NUM];
  if (ScanTE(devs) == 1) {
    if (devs[0].nProdVer == 0) {
      device_TE_B = OpenTE_B(I3_TE_Q1, 0);
      camera_model = I3_TE_Q1;
    } else if (devs[0].nProdVer == I3_TE_M1) {
      device_TE_B = OpenTE_B(I3_TE_M1, 0);
      camera_model = I3_TE_M1;
    } else {
      cout << "Unknown product number!" << endl;
      return -1;
    }
  } else {
    cout << "Cannot find device!" << endl;
    return -1;
  }

  if (device_TE_B) {
    if (camera_model == I3_TE_Q1)
      cout << "Found model Q1." << endl;
    else if (camera_model == I3_TE_M1)
      cout << "Found model M1." << endl;

    cout << "Initializing..." << endl;
    alarm(20); // Timeout after X seconds
    if (device_TE_B->ReadFlashData() != 1) {
      cerr << "Cannot read flash data" << endl;
      return -1;
    } else {
      alarm(0);
      cout << "Initialized!" << endl;
      calibration_ready = true;
      pthread_t thrd;
      pthread_create(&thrd, NULL, mainThread, 0);
      pthread_join(thrd, NULL);
    }
  } else {
    cout << "Could not open device!" << endl;
    return -1;
  }
  return 0;
}
