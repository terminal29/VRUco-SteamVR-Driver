#include <iostream>
#include <chrono>
#include <SharedPacket.h>

#include "ps3eye_context.h"

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/calib3d.hpp>

#include <aruco/aruco.h>

#include "RoomSetup.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <thread> 

auto WriteLog = [](std::string info) {std::cout << info << std::endl;};

auto current_time = []() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
};

bool loadCalibration(std::string name, cv::Mat& camera_matrix, cv::Mat& distance_coefficients) {
	std::ifstream instream(name);
	if (instream) {
		uint16_t rows;
		uint16_t cols;
		instream >> rows;
		instream >> cols;

		camera_matrix = cv::Mat(cv::Size(rows, cols), CV_64F);

		for (int r = 0; r < rows; r++)
		{
			for (int c = 0; c < cols; c++)
			{
				double read = 0.f;
				instream >> read;
				camera_matrix.at<double>(r, c) = read;
			}
		}

		instream >> rows;
		instream >> cols;

		distance_coefficients = cv::Mat::zeros(cv::Size(cols, rows), CV_64F);

		for (int r = 0; r < rows; r++)
		{
			for (int c = 0; c < cols; c++)
			{
				double read = 0.f;
				instream >> read;
				distance_coefficients.at<double>(r, c) = read;
			}
		}
		instream.close();
		return true;
	}
	return false;
}

int main()
{
	bool set_centre = false;
	bool has_finished_startup = false;
	bool exit_requested = false;
	bool wants_calibration = false;
	bool has_ransac = false;

	float predefined_center[3] = { 0,1.75, 0 };
	float centre_offset[3] = { 0 };
	aruco::MarkerMap marker_map;

	std::thread runner_thread([&]() {
		SharedPacket::DataPacket* dp = SharedPacket::getSharedDataPacket();
		WriteLog("Obtained shared memory pointer.");

		// ###################  Setup OpenCV  ####################
		const int CAMERA_WIDTH = 640;
		const int CAMERA_HEIGHT = 480;
		const int CAMERA_FPS = 60;

		uint8_t camera_data_raw[CAMERA_WIDTH * CAMERA_HEIGHT * 3];
		IplImage* camera_data_ipl = cvCreateImage(cvSize(CAMERA_WIDTH, CAMERA_HEIGHT), 8, 3);
		cv::Mat current_frame, camera_matrix, distance_coefficients;
		const float aruco_square_dims = 0.0744; //0.1664f; // metres (Pre measured)
		WriteLog("Loading calibration file ps3_eye_calibration.txt");
		if (!loadCalibration("ps3_eye_calibration.txt", camera_matrix, distance_coefficients)) {
			WriteLog("Cannot find or load calibration file 'ps3_eye_calibration.txt'");
			WriteLog("Exiting...");
			return -1;
		}
		aruco::MarkerDetector marker_detector(aruco::Dictionary::ARUCO_MIP_36h12);
		marker_detector.getParameters().setAutoSizeSpeedUp(true, 0.1f);
		marker_detector.setDetectionMode(aruco::DetectionMode::DM_VIDEO_FAST);
		marker_detector.getParameters().maxThreads = -1;
		//marker_detector.getParameters().lowResMarkerSize = 5;
	//	marker_detector.getParameters().enclosedMarker = true;

		aruco::CameraParameters camera_params{ camera_matrix, distance_coefficients, cv::Size(CAMERA_WIDTH, CAMERA_HEIGHT) };


		// ###################  Setup Libusb & Ps3Eye  ####################
		WriteLog("Initialising libusb");
		libusb_init(NULL);
		ps3eye_context ctx(CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);
		if (!ctx.hasDevices()) {
			WriteLog("Cannot find at least 1 PS3 Eye camera connected.");
			WriteLog("Exiting...");
			return -1;
		}
		WriteLog(std::to_string(ctx.devices.size()) + " ps3 eye(s) connected!");
		WriteLog("Starting stream using first ps3 eye.");
		ctx.eye->start();
		ctx.eye->setAutoWhiteBalance(true);
		ctx.eye->setAutogain(true);
		ctx.eye->setSharpness(255);
		has_finished_startup = true;
		const int max_idx = 5;
		int cur_idx = 0;
		glm::quat previous_quaternions[max_idx];
		cv::Vec3f previous_positions[max_idx];
		
		//cv::namedWindow("Marker View");
		std::chrono::milliseconds now = current_time();
		int fps = 0;
		while (!exit_requested) {
			if (wants_calibration) {
				marker_map = RoomSetup::GetMarkerMap(ctx, camera_params, "ARUCO_MIP_36h12", aruco_square_dims);





				wants_calibration = false;
			}
			if (!dp->new_data_available) {
				ctx.eye->getFrame(camera_data_raw);
				cvSetData(camera_data_ipl, camera_data_raw, camera_data_ipl->widthStep);
				current_frame = cv::cvarrToMat(camera_data_ipl);

				std::vector<aruco::Marker> markers = marker_detector.detect(current_frame, camera_params, aruco_square_dims);
				
				std::vector<cv::Point2f> markers_image_points;
				std::vector<cv::Point3f> markers_real_points;
				
				for (aruco::Marker& marker : markers) {
					
					//marker.draw(current_frame);
					std::vector<cv::Point2f> current_marker_image_points;
					cv::projectPoints(marker.get3DPoints(), marker.Rvec, marker.Tvec, camera_matrix, distance_coefficients, current_marker_image_points);
					for (auto mapped_marker : marker_map) {
						if (marker.id == mapped_marker.id) {
							std::vector<cv::Point3f> current_marker_real_points = mapped_marker.points;
							markers_image_points.insert(markers_image_points.end(),current_marker_image_points.begin(), current_marker_image_points.end());
							markers_real_points.insert(markers_real_points.end(),current_marker_real_points.begin(), current_marker_real_points.end());
							break;
						}
					}
				}

				if (markers_image_points.size() > 0) {
					cv::Mat rvec, tvec;
					cv::solvePnPRansac(markers_real_points, markers_image_points, camera_matrix, distance_coefficients, rvec, tvec, has_ransac);
					has_ransac = true;
					cv::Mat rotation;
					cv::Rodrigues(rvec, rotation);
					tvec = -(rotation.t()) * tvec;
					cv::Vec3f position = tvec;

					cv::Vec3f v_rvec = rvec;
					float theta = sqrt(v_rvec[0] * v_rvec[0] + v_rvec[1] * v_rvec[1] + v_rvec[2] * v_rvec[2]);

					glm::vec3 axis{ v_rvec[0] / theta, v_rvec[1] / theta, v_rvec[2] / theta };

					glm::quat quat = glm::normalize(glm::quat(0, 1, 0, 0)) * glm::angleAxis(theta, axis);
					quat = glm::quat(-quat.w, quat.x, quat.y, quat.z);

					if (set_centre) {
						centre_offset[0] = -position[0];
						centre_offset[1] = -position[1];
						centre_offset[2] = -position[2];
						set_centre = false;
					}

					cv::Vec3f hmd_position_ig;
					hmd_position_ig[0] = position[0] + predefined_center[0] + centre_offset[0];
					hmd_position_ig[1] = position[1] + predefined_center[1] + centre_offset[1];
					hmd_position_ig[2] = position[2] + predefined_center[2] + centre_offset[2];

					dp->hmd_position[0] = hmd_position_ig[0];
					dp->hmd_position[1] = hmd_position_ig[1];
					dp->hmd_position[2] = hmd_position_ig[2];

					dp->hmd_quaternion[0] = quat.x;
					dp->hmd_quaternion[1] = quat.y;
					dp->hmd_quaternion[2] = quat.z;
					dp->hmd_quaternion[3] = quat.w;

					dp->new_data_available = true;
				}
				//cv::imshow("Marker View", current_frame);
				//cv::waitKey(1);

				fps++;
				if (now.count() + 1000 < current_time().count()) {
					now = current_time();
					WriteLog("FPS: " + std::to_string(fps));
					fps = 0;
				}
			}
		}
	});

	while (!has_finished_startup) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); };
	while (!exit_requested) {
		std::string input;
		std::cin >> input;
		if (input == "exit") {
			exit_requested = true;
		}
		else if (input == "centre") {
			set_centre = true;
		}
		else if (input == "calibrate") {
			wants_calibration = true;
		}
		else {
			WriteLog("Invalid Input");
		}
	}

	runner_thread.join();
    return 0;
}

