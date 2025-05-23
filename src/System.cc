/**
 * This file is part of ORB-SLAM2.
 *
 * Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University
 * of Zaragoza) For more information see <https://github.com/raulmur/ORB_SLAM2>
 *
 * ORB-SLAM2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ORB-SLAM2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "System.h"
#include "Converter.h"
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <iomanip>
#include <pangolin/pangolin.h>
#include <thread>

namespace ORB_SLAM2 {

System::System(const string &strVocFile, const string &strSettingsFile,
               const eSensor sensor, const bool bUseViewer)
    : mSensor(sensor), mpViewer(static_cast<Viewer *>(NULL)), mbReset(false) {
  // Output welcome message
  cout << endl
       << "ORB-SLAM2 Copyright (C) 2014-2016 Raul Mur-Artal, University of "
          "Zaragoza."
       << endl
       << "This program comes with ABSOLUTELY NO WARRANTY;" << endl
       << "This is free software, and you are welcome to redistribute it"
       << endl
       << "under certain conditions. See LICENSE.txt." << endl
       << endl;

  SetSlamParams(strSettingsFile);

  // Load ORB Vocabulary
  cout << endl
       << "[system] Loading ORB Vocabulary. This could take a while..." << endl;
  mpVocabulary = new ORBVocabulary();
  // bool bVocLoad = mpVocabulary->loadFromTextFile(strVocFile);
  bool bVocLoad = mpVocabulary->loadFromBinFile(strVocFile);
  if (!bVocLoad) {
    cerr << "[system] Wrong path to vocabulary. " << endl;
    cerr << "[system] Falied to open at: " << strVocFile << endl;
    exit(-1);
  }
  cout << "[system] Vocabulary loaded!" << endl << endl;

  // Create KeyFrame Database
  mpKeyFrameDatabase = new KeyFrameDatabase(*mpVocabulary);

  // Create the Map
  mpMap = new Map();

  // Create Drawers. These are used by the Viewer
  mpFrameDrawer = new FrameDrawer(mpMap);
  mpMapDrawer = new MapDrawer(mpMap, strSettingsFile);

  // Initialize the Tracking thread
  //(it will live in the main thread of execution, the one that called this
  // constructor)
  mpTracker = new Tracking(this, mpVocabulary, mpFrameDrawer, mpMapDrawer,
                           mpMap, mpKeyFrameDatabase, strSettingsFile, mSensor);

  // Initialize the Local Mapping thread and launch
  mpLocalMapper = new LocalMapping(mpMap, mSensor == MONOCULAR);
  mptLocalMapping = new thread(&ORB_SLAM2::LocalMapping::Run, mpLocalMapper);

  // Initialize the Loop Closing thread and launch
  mpLoopCloser = new LoopClosing(mpMap, mpKeyFrameDatabase, mpVocabulary,
                                 mSensor != MONOCULAR);
  mptLoopClosing = new thread(&ORB_SLAM2::LoopClosing::Run, mpLoopCloser);

  if (mbOnlyRelocalization) {
    std::cout << "[system] load map from : " << mMapFile << std::endl;
    if (LoadMap(mMapFile)) {
      ActivateLocalizationMode();
      // mpTracker->mState = Tracking::eTrackingState::LOST;
    }
  }

  // Initialize the Viewer thread and launch
  if (bUseViewer) {
    mpViewer = new Viewer(this, mpFrameDrawer, mpMapDrawer, mpTracker,
                          strSettingsFile);
    mptViewer = new thread(&Viewer::Run, mpViewer);
    mpTracker->SetViewer(mpViewer);
  }

  // Set pointers between threads
  mpTracker->SetLocalMapper(mpLocalMapper);
  mpTracker->SetLoopClosing(mpLoopCloser);

  mpLocalMapper->SetTracker(mpTracker);
  mpLocalMapper->SetLoopCloser(mpLoopCloser);

  mpLoopCloser->SetTracker(mpTracker);
  mpLoopCloser->SetLocalMapper(mpLocalMapper);
}

void System::SetSlamParams(const string &strSettingsFile) {
  cv::FileStorage fsSettings(strSettingsFile.c_str(), cv::FileStorage::READ);
  if (!fsSettings.isOpened()) {
    std::cerr << "[system] Failed to open settings file at: " << strSettingsFile
              << endl;
    return;
  }
  fsSettings["DeactivateLocalizationMode"] >> mbDeactivateLocalizationMode;
  fsSettings["ActivateLocalizationMode"] >> mbActivateLocalizationMode;
  fsSettings["OnlyRelocalization"] >> mbOnlyRelocalization;

  cv::FileNode mapfilen = fsSettings["map.mapfile"];
  if (!mapfilen.empty()) {
    mMapFile = mapfilen.string();
  }
}

cv::Mat System::TrackStereo(const cv::Mat &imLeft, const cv::Mat &imRight,
                            const double &timestamp) {
  if (mSensor != STEREO) {
    cerr << "[system] ERROR: you called TrackStereo but input sensor was not "
            "set to "
            "STEREO."
         << endl;
    exit(-1);
  }

  // Check mode change
  {
    unique_lock<mutex> lock(mMutexMode);
    if (mbActivateLocalizationMode) {
      mpLocalMapper->RequestStop();

      // Wait until Local Mapping has effectively stopped
      while (!mpLocalMapper->isStopped()) {
        usleep(1000);
      }

      mpTracker->InformOnlyTracking(true);
      mbActivateLocalizationMode = false;
    }
    if (mbDeactivateLocalizationMode) {
      mpTracker->InformOnlyTracking(false);
      mpLocalMapper->Release();
      mbDeactivateLocalizationMode = false;
    }
  }

  // Check reset
  {
    unique_lock<mutex> lock(mMutexReset);
    if (mbReset) {
      mpTracker->Reset();
      mbReset = false;
    }
  }

  cv::Mat Tcw = mpTracker->GrabImageStereo(imLeft, imRight, timestamp);

  unique_lock<mutex> lock2(mMutexState);
  mTrackingState = mpTracker->mState;
  mTrackedMapPoints = mpTracker->mCurrentFrame.mvpMapPoints;
  mTrackedKeyPointsUn = mpTracker->mCurrentFrame.mvKeysUn;
  return Tcw;
}

cv::Mat System::TrackRGBD(const cv::Mat &im, const cv::Mat &depthmap,
                          const double &timestamp) {
  if (mSensor != RGBD) {
    cerr << "[system] ERROR: you called TrackRGBD but input sensor was not set "
            "to RGBD."
         << endl;
    exit(-1);
  }

  // Check mode change
  {
    unique_lock<mutex> lock(mMutexMode);
    if (mbActivateLocalizationMode) {
      mpLocalMapper->RequestStop();

      // Wait until Local Mapping has effectively stopped
      while (!mpLocalMapper->isStopped()) {
        usleep(1000);
      }

      mpTracker->InformOnlyTracking(true);
      mbActivateLocalizationMode = false;
    }
    if (mbDeactivateLocalizationMode) {
      mpTracker->InformOnlyTracking(false);
      mpLocalMapper->Release();
      mbDeactivateLocalizationMode = false;
    }
  }

  // Check reset
  {
    unique_lock<mutex> lock(mMutexReset);
    if (mbReset) {
      mpTracker->Reset();
      mbReset = false;
    }
  }

  cv::Mat Tcw = mpTracker->GrabImageRGBD(im, depthmap, timestamp);

  unique_lock<mutex> lock2(mMutexState);
  mTrackingState = mpTracker->mState;
  mTrackedMapPoints = mpTracker->mCurrentFrame.mvpMapPoints;
  mTrackedKeyPointsUn = mpTracker->mCurrentFrame.mvKeysUn;
  return Tcw;
}

cv::Mat System::TrackMonocular(const cv::Mat &im, const double &timestamp) {
  if (mSensor != MONOCULAR) {
    cerr << "[system] ERROR: you called TrackMonocular but input sensor was "
            "not set to "
            "Monocular."
         << endl;
    exit(-1);
  }
  // Check mode change
  {
    std::cout << "[system] Check mode change" << std::endl;
    unique_lock<mutex> lock(mMutexMode);
    if (mbActivateLocalizationMode) {
      mpLocalMapper->RequestStop();

      // Wait until Local Mapping has effectively stopped
      while (!mpLocalMapper->isStopped()) {
        usleep(1000);
      }

      mpTracker->InformOnlyTracking(false);
      mbActivateLocalizationMode = false;
    }
    if (mbDeactivateLocalizationMode) {
      mpTracker->InformOnlyTracking(false);
      mpLocalMapper->Release();
      mbDeactivateLocalizationMode = false;
    }
  }

  // Check reset
  {
    unique_lock<mutex> lock(mMutexReset);
    if (mbReset) {
      mpTracker->Reset();
      mbReset = false;
    }
  }
  std::cout << "[system] TrackMonocular" << std::endl;
  cv::Mat Tcw = mpTracker->GrabImageMonocular(im, timestamp);
  // std::cout << "[system] Tcw" << Tcw << std::endl;

  unique_lock<mutex> lock2(mMutexState);
  mTrackingState = mpTracker->mState;
  mTrackedMapPoints = mpTracker->mCurrentFrame.mvpMapPoints;
  mTrackedKeyPointsUn = mpTracker->mCurrentFrame.mvKeysUn;

  return Tcw;
}

void System::ActivateLocalizationMode() {
  unique_lock<mutex> lock(mMutexMode);
  mbActivateLocalizationMode = true;
}

void System::DeactivateLocalizationMode() {
  unique_lock<mutex> lock(mMutexMode);
  mbDeactivateLocalizationMode = true;
}

bool System::MapChanged() {
  static int n = 0;
  int curn = mpMap->GetLastBigChangeIdx();
  if (n < curn) {
    n = curn;
    return true;
  } else
    return false;
}

void System::Reset() {
  unique_lock<mutex> lock(mMutexReset);
  mbReset = true;
}

void System::Shutdown() {
  mpLocalMapper->RequestFinish();
  mpLoopCloser->RequestFinish();
  if (mpViewer) {
    mpViewer->RequestFinish();
    while (!mpViewer->isFinished())
      usleep(5000);
  }

  // Wait until all thread have effectively stopped
  while (!mpLocalMapper->isFinished() || !mpLoopCloser->isFinished() ||
         mpLoopCloser->isRunningGBA()) {
    usleep(5000);
  }

  //   if (mpViewer) {
  //     pangolin::BindToContext("ORB-SLAM2: Map Viewer");
  //   }
}

void System::SaveTrajectoryTUM(const string &filename) {
  cout << endl
       << "[system] Saving camera trajectory to " << filename << " ..." << endl;
  if (mSensor == MONOCULAR) {
    cerr << "[system] ERROR: SaveTrajectoryTUM cannot be used for monocular."
         << endl;
    return;
  }

  vector<KeyFrame *> vpKFs = mpMap->GetAllKeyFrames();
  sort(vpKFs.begin(), vpKFs.end(), KeyFrame::lId);

  // Transform all keyframes so that the first keyframe is at the origin.
  // After a loop closure the first keyframe might not be at the origin.
  cv::Mat Two = vpKFs[0]->GetPoseInverse();

  ofstream f;
  f.open(filename.c_str());
  f << fixed;

  // Frame pose is stored relative to its reference keyframe (which is optimized
  // by BA and pose graph). We need to get first the keyframe pose and then
  // concatenate the relative transformation. Frames not localized (tracking
  // failure) are not saved.

  // For each frame we have a reference keyframe (lRit), the timestamp (lT) and
  // a flag which is true when tracking failed (lbL).
  list<ORB_SLAM2::KeyFrame *>::iterator lRit = mpTracker->mlpReferences.begin();
  list<double>::iterator lT = mpTracker->mlFrameTimes.begin();
  list<bool>::iterator lbL = mpTracker->mlbLost.begin();
  for (list<cv::Mat>::iterator lit = mpTracker->mlRelativeFramePoses.begin(),
                               lend = mpTracker->mlRelativeFramePoses.end();
       lit != lend; lit++, lRit++, lT++, lbL++) {
    if (*lbL)
      continue;

    KeyFrame *pKF = *lRit;

    cv::Mat Trw = cv::Mat::eye(4, 4, CV_32F);

    // If the reference keyframe was culled, traverse the spanning tree to get a
    // suitable keyframe.
    while (pKF->isBad()) {
      Trw = Trw * pKF->mTcp;
      pKF = pKF->GetParent();
    }

    Trw = Trw * pKF->GetPose() * Two;

    cv::Mat Tcw = (*lit) * Trw;
    cv::Mat Rwc = Tcw.rowRange(0, 3).colRange(0, 3).t();
    cv::Mat twc = -Rwc * Tcw.rowRange(0, 3).col(3);

    vector<float> q = Converter::toQuaternion(Rwc);

    f << setprecision(6) << *lT << " " << setprecision(9) << twc.at<float>(0)
      << " " << twc.at<float>(1) << " " << twc.at<float>(2) << " " << q[0]
      << " " << q[1] << " " << q[2] << " " << q[3] << endl;
  }
  f.close();
  cout << endl << "[system] trajectory saved!" << endl;
}

void System::SaveKeyFrameTrajectoryTUM(const string &filename) {
  cout << endl
       << "[system] Saving keyframe trajectory to " << filename << " ..."
       << endl;

  vector<KeyFrame *> vpKFs = mpMap->GetAllKeyFrames();
  sort(vpKFs.begin(), vpKFs.end(), KeyFrame::lId);

  // Transform all keyframes so that the first keyframe is at the origin.
  // After a loop closure the first keyframe might not be at the origin.
  // cv::Mat Two = vpKFs[0]->GetPoseInverse();

  ofstream f;
  f.open(filename.c_str());
  f << fixed;

  for (size_t i = 0; i < vpKFs.size(); i++) {
    KeyFrame *pKF = vpKFs[i];

    // pKF->SetPose(pKF->GetPose()*Two);

    if (pKF->isBad())
      continue;

    cv::Mat R = pKF->GetRotation().t();
    vector<float> q = Converter::toQuaternion(R);
    cv::Mat t = pKF->GetCameraCenter();
    f << setprecision(6) << pKF->mTimeStamp << setprecision(7) << " "
      << t.at<float>(0) << " " << t.at<float>(1) << " " << t.at<float>(2) << " "
      << q[0] << " " << q[1] << " " << q[2] << " " << q[3] << endl;
  }

  f.close();
  cout << endl << "[system] trajectory saved!" << endl;
}

void System::SaveTrajectoryKITTI(const string &filename) {
  cout << endl << "Saving camera trajectory to " << filename << " ..." << endl;
  if (mSensor == MONOCULAR) {
    cerr << "ERROR: SaveTrajectoryKITTI cannot be used for monocular." << endl;
    return;
  }

  vector<KeyFrame *> vpKFs = mpMap->GetAllKeyFrames();
  sort(vpKFs.begin(), vpKFs.end(), KeyFrame::lId);

  // Transform all keyframes so that the first keyframe is at the origin.
  // After a loop closure the first keyframe might not be at the origin.
  cv::Mat Two = vpKFs[0]->GetPoseInverse();

  ofstream f;
  f.open(filename.c_str());
  f << fixed;

  // Frame pose is stored relative to its reference keyframe (which is optimized
  // by BA and pose graph). We need to get first the keyframe pose and then
  // concatenate the relative transformation. Frames not localized (tracking
  // failure) are not saved.

  // For each frame we have a reference keyframe (lRit), the timestamp (lT) and
  // a flag which is true when tracking failed (lbL).
  list<ORB_SLAM2::KeyFrame *>::iterator lRit = mpTracker->mlpReferences.begin();
  list<double>::iterator lT = mpTracker->mlFrameTimes.begin();
  for (list<cv::Mat>::iterator lit = mpTracker->mlRelativeFramePoses.begin(),
                               lend = mpTracker->mlRelativeFramePoses.end();
       lit != lend; lit++, lRit++, lT++) {
    ORB_SLAM2::KeyFrame *pKF = *lRit;

    cv::Mat Trw = cv::Mat::eye(4, 4, CV_32F);

    while (pKF->isBad()) {
      //  cout << "bad parent" << endl;
      Trw = Trw * pKF->mTcp;
      pKF = pKF->GetParent();
    }

    Trw = Trw * pKF->GetPose() * Two;

    cv::Mat Tcw = (*lit) * Trw;
    cv::Mat Rwc = Tcw.rowRange(0, 3).colRange(0, 3).t();
    cv::Mat twc = -Rwc * Tcw.rowRange(0, 3).col(3);

    f << setprecision(9) << Rwc.at<float>(0, 0) << " " << Rwc.at<float>(0, 1)
      << " " << Rwc.at<float>(0, 2) << " " << twc.at<float>(0) << " "
      << Rwc.at<float>(1, 0) << " " << Rwc.at<float>(1, 1) << " "
      << Rwc.at<float>(1, 2) << " " << twc.at<float>(1) << " "
      << Rwc.at<float>(2, 0) << " " << Rwc.at<float>(2, 1) << " "
      << Rwc.at<float>(2, 2) << " " << twc.at<float>(2) << endl;
  }
  f.close();
  cout << endl << "trajectory saved!" << endl;
}

void System::SaveMap(const string &filename) {
  std::ofstream out(filename, std::ios_base::binary);
  if (!out) {
    cerr << "[system] Cannot Write to Mapfile: " << mMapFile << std::endl;
    exit(-1);
  }
  std::cout << "[system] Saving Mapfile: " << mMapFile << std::flush;
  boost::archive::binary_oarchive oa(out, boost::archive::no_header);
  std::vector<KeyFrame *> vpKFs = mpMap->GetAllKeyFrames();
  std::vector<MapPoint *> vMapPoints = mpMap->GetAllMapPoints();
  sort(vpKFs.begin(), vpKFs.end(), KeyFrame::lId);
  oa << vpKFs;
  oa << vMapPoints;
  std::cout << "[system] mapfile saved successfully!" << std::endl;
  out.close();
}

bool System::LoadMap(const string &filename) {
  if (filename.empty()) {
    std::cout << "[system] Mapfile is empty" << std::endl;
    return false;
  }
  std::ifstream in(filename, std::ios_base::binary);
  if (!in) {
    cerr << "[system] Cannot Open Mapfile: " << mMapFile << ", Create a new one"
         << std::endl;
    return false;
  }
  std::vector<KeyFrame *> vpKFs;
  std::vector<MapPoint *> vMapPoints;
  boost::archive::binary_iarchive ia(in, boost::archive::no_header);
  ia >> vpKFs;
  ia >> vMapPoints;
  std::cout << "[system] Mapfile loaded successfully from " << filename
            << std::endl;
  std::cout << "[system] Map Reconstructing" << flush;
  const size_t total_size = vpKFs.size();
  std::atomic_size_t count = 0;
  std::atomic_bool is_finished = false;
  std::thread load_keyframe(&System::LoadKeyFrames, this, vpKFs, mpMap, &count,
                            &is_finished);
  while (!is_finished) {
    usleep(30000);
  }
  load_keyframe.join();
  std::cout << "[system] Insert KeyFrame Current/Total: " << (count - 1) << "/"
            << total_size << " , Done !" << std::endl;

  std::thread load_map_point_thread(&System::LoadMapPoints, this, vMapPoints,
                                    mpMap);
  for (size_t i = 0; i < vpKFs.size(); i++) {
    vpKFs[i]->UpdateConnections();
  }
  load_map_point_thread.join();
  in.close();
  return true;
}

int System::GetTrackingState() {
  unique_lock<mutex> lock(mMutexState);
  return mTrackingState;
}

vector<MapPoint *> System::GetTrackedMapPoints() {
  unique_lock<mutex> lock(mMutexState);
  return mTrackedMapPoints;
}

vector<cv::KeyPoint> System::GetTrackedKeyPointsUn() {
  unique_lock<mutex> lock(mMutexState);
  return mTrackedKeyPointsUn;
}

void System::LoadMapPoints(const std::vector<MapPoint *> &saved_map_points,
                           Map *pMap) {
  for (size_t i = 0; i < saved_map_points.size(); i++) {
    saved_map_points[i]->ComputeDistinctiveDescriptors();
    saved_map_points[i]->UpdateNormalAndDepth();
    pMap->AddMapPoint(saved_map_points[i]);
  }
  const auto &points = pMap->GetAllMapPoints();
  std::cout << "[system] Load Map Points size : " << points.size() << std::endl;
}

void System::LoadKeyFrames(const std::vector<KeyFrame *> &vpKFs, Map *pMap,
                           std::atomic_size_t *const pCount,
                           std::atomic_bool *const p_is_finished) {
  for (auto &pKF : vpKFs) {
    if (pKF == nullptr || pKF->isBad()) {
      continue;
    }
    AddKeyFrame(pKF, pMap);
    pCount->fetch_add(1);
  }
  *p_is_finished = true;
}

void System::AddKeyFrame(KeyFrame *keyframe, Map *pMap) {
  // Frame frame(cv::Size((int)mWidth, (int)mHeight), keyframe->GetPose(),
  //             keyframe->mvKeys, keyframe->mvKeysUn, keyframe->mDescriptors,
  //             keyframe->_globaldescriptors, keyframe->mTimeStamp,
  //             mpVocabulary, mk, mbf);

  // KeyFrame *pKF = KeyFrame::CreateNewKeyFrame(frame, pMap,
  // mpKeyFrameDatabase); pKF->SetPose(keyframe->GetPose());
  // std::vector<cv::Mat> vDesc =
  // Converter::toDescriptorVector(pKF->mDescriptors);
  // mpVocabulary->transform(pKF->mDescriptors, 4, pKF->mBowVec, pKF->mFeatVec);

  pMap->AddKeyFrame(keyframe);
  mpKeyFrameDatabase->add(keyframe);
  std::vector<MapPoint *> vpMaps = keyframe->GetMapPointMatches();

  for (unsigned i = 0; i < vpMaps.size(); i++) {
    MapPoint *pMP = vpMaps[i];
    if (pMP == nullptr || pMP->isBad()) {
      std::cout << "nullptr/bad map point detected while adding a keyframe!"
                << std::endl;
      continue;
    }
    pMP->AddObservation(keyframe, i);
    keyframe->AddMapPoint(pMP, i);
    pMap->AddMapPoint(pMP);
  }
}

} // namespace ORB_SLAM2
