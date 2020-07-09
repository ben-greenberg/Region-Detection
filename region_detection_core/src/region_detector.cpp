/*
 * @author Jorge Nicho
 * @file region_detector.cpp
 * @date Jun 4, 2020
 * @copyright Copyright (c) 2020, Southwest Research Institute
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2020, Southwest Research Institute
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *       * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *       * Neither the name of the Southwest Research Institute, nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <yaml-cpp/yaml.h>

#include <opencv2/highgui.hpp>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>

#include <log4cxx/basicconfigurator.h>
#include <log4cxx/patternlayout.h>
#include <log4cxx/consoleappender.h>

#include <pcl/common/transforms.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/features/normal_3d.h>
#include <pcl/conversions.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/surface/concave_hull.h>

#include "region_detection_core/region_detector.h"

static const std::map<int,int> DILATION_TYPES = {{0, cv::MORPH_RECT}, {1, cv::MORPH_CROSS},{2, cv::MORPH_ELLIPSE}};
static cv::RNG RANDOM_NUM_GEN(12345);
static const int MIN_PIXEL_DISTANCE = 1; // used during interpolation in pixel space
static const double MIN_POINT_DIST = 1e-8;

log4cxx::LoggerPtr createDefaultLogger(const std::string& logger_name)
{
  using namespace log4cxx;
  PatternLayoutPtr pattern_layout(new PatternLayout("[\%-5p] [\%c](L:\%L): \%m\%n"));
  ConsoleAppenderPtr console_appender(new ConsoleAppender(pattern_layout));
  log4cxx::LoggerPtr logger(Logger::getLogger(logger_name));
  logger->addAppender(console_appender);
  logger->setLevel(Level::getInfo());
  return logger;
}

Eigen::Matrix3d toRotationMatrix(const Eigen::Vector3d& vx, const Eigen::Vector3d& vy,
                                                    const Eigen::Vector3d& vz)
{
  using namespace Eigen;
  Matrix3d rot;
  rot.block(0,0,1,3) = Vector3d(vx.x(), vy.x(), vz.x()).array().transpose();
  rot.block(1,0,1,3) = Vector3d(vx.y(), vy.y(), vz.y()).array().transpose();
  rot.block(2,0,1,3) = Vector3d(vx.z(), vy.z(), vz.z()).array().transpose();
  return rot;
}

template <typename T>
std::vector<T> linspace(T a, T b, size_t N)
{
    T h = (b - a) / static_cast<T>(N-1);
    std::vector<T> xs(N);
    typename std::vector<T>::iterator x;
    T val;
    for (x = xs.begin(), val = a; x != xs.end(); ++x, val += h)
        *x = val;
    return xs;
}

pcl::PointCloud<pcl::PointXYZ> convert2DContourToCloud(const std::vector<cv::Point>& contour_2d)
{
  pcl::PointCloud<pcl::PointXYZ> contour_3d;
  pcl::PointXYZ p_3d;
  for(const auto& p_2d :contour_2d )
  {
    p_3d.x = p_2d.x;
    p_3d.y = p_2d.y;
    p_3d.z = 0.0;
    contour_3d.push_back(p_3d);
  }
  return contour_3d;
}

std::vector<cv::Point> convertCloudTo2DContour(const pcl::PointCloud<pcl::PointXYZ>& contour_3d)
{
  std::vector<cv::Point> contour_2d;
  cv::Point p_2d;
  for(const auto& p_3d :contour_3d )
  {
    p_2d.x = p_3d.x;
    p_2d.y = p_3d.y;
    contour_2d.push_back(p_2d);
  }
  return contour_2d;
}

void dowsampleCloud(pcl::PointCloud<pcl::PointXYZ>& cloud, double leafsize = 1.0)
{
  using namespace pcl;
  VoxelGrid<PointXYZ> voxelizer;
  voxelizer.setLeafSize(leafsize, leafsize, leafsize);
  voxelizer.setInputCloud(cloud.makeShared());
  cloud.clear();
  voxelizer.filter(cloud);
}

pcl::PointCloud<pcl::PointXYZ>::Ptr concaveHullSimplification(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr closed_polygon,
                                                                           double segment_length)
{
  pcl::PointCloud<pcl::PointXYZ>::Ptr simplified_polygon = boost::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  pcl::ConcaveHull<pcl::PointXYZ> chull;
  chull.setInputCloud(closed_polygon);
  chull.setAlpha(segment_length);
  chull.reconstruct(*simplified_polygon);
  return simplified_polygon;
}

namespace region_detection_core
{

pcl::PointCloud<pcl::PointXYZ> RegionDetector::sequence(pcl::PointCloud<pcl::PointXYZ>::ConstPtr points,
                                                                double epsilon)
{
  using namespace pcl;
  std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> sequenced_points_vec;

  // build reordering kd tree
  pcl::KdTreeFLANN<pcl::PointXYZ> sequencing_kdtree;
  sequencing_kdtree.setEpsilon(epsilon);
  sequencing_kdtree.setSortedResults(true);

  auto& cloud = *points;
  std::vector<int> sequenced_indices, unsequenced_indices;
  sequenced_indices.reserve(cloud.size());
  unsequenced_indices.resize(cloud.size());
  std::iota(unsequenced_indices.begin(), unsequenced_indices.end(),0);

  // cloud of all sequenced points
  pcl::PointCloud<pcl::PointXYZ> sequenced_points;
  sequenced_points.reserve(cloud.size());

  // search variables
  int search_point_idx = 0;
  const int max_iters = cloud.size();
  PointXYZ search_point = cloud[search_point_idx];
  PointXYZ start_point = search_point;
  PointXYZ closest_point;
  int iter_count = 0;

  // now reorder based on proximity
  while(iter_count <= max_iters)
  {
    iter_count++;

    // remove from active
    unsequenced_indices.erase(std::remove(unsequenced_indices.begin(), unsequenced_indices.end(),search_point_idx));

    if(unsequenced_indices.empty())
    {
      break;
    }

    // set tree inputs;
    IndicesConstPtr cloud_indices = boost::make_shared<const std::vector<int>>(unsequenced_indices);
    sequencing_kdtree.setInputCloud(points,cloud_indices);
    sequencing_kdtree.setSortedResults(true);

    // find next point
    const int k_points = 1;
    std::vector<int> k_indices(k_points);
    std::vector<float> k_sqr_distances(k_points);
    int points_found = sequencing_kdtree.nearestKSearch(search_point,k_points, k_indices, k_sqr_distances);
    if(points_found < k_points)
    {
      std::string err_msg = boost::str(boost::format(
          "NearestKSearch Search did not find any points close to [%f, %f, %f]") %
                                       search_point.x % search_point.y % search_point.z);
      LOG4CXX_WARN(logger_,err_msg);
      break;
    }
    // saving search point
    if(sequenced_indices.empty())
    {
      sequenced_indices.push_back(search_point_idx); // insert first point
      sequenced_points.push_back(search_point);
    }

    // insert new point if it has not been visited
    if(std::find(sequenced_indices.begin(), sequenced_indices.end(),k_indices[0]) != sequenced_indices.end())
    {
      // there should be no more points to add
      LOG4CXX_WARN(logger_,"Found repeated point during reordering stage, should not happen but proceeding");
      continue;
    }

    // check if found point is further away than the start point
    closest_point = cloud[k_indices[0]];
    start_point = cloud[sequenced_indices[0]];
    Eigen::Vector3d diff = (start_point.getArray3fMap() - closest_point.getArray3fMap()).cast<double>();
    if(diff.norm() < std::sqrt(k_sqr_distances[0]))
    {
      // reversing will allow adding point from the other end
      std::reverse(sequenced_indices.begin(),sequenced_indices.end());
      std::reverse(sequenced_points.begin(),sequenced_points.end());
    }

    // set next search point variables
    search_point_idx = k_indices[0];
    search_point = closest_point;

    // add to visited
    sequenced_indices.push_back(k_indices[0]);
    sequenced_points.push_back(closest_point);
  }

  LOG4CXX_DEBUG(logger_,"Sequenced " << sequenced_points.size() << " points from "<< cloud.size());
  return sequenced_points;
}

std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> RegionDetector::split(const pcl::PointCloud<pcl::PointXYZ>& sequenced_points,
                                                                       double split_dist)
{
  using namespace pcl;

  // splitting
  std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> sequenced_points_vec;
  std::size_t start_idx = 0;
  std::size_t end_idx;
  for(std::size_t i = 0; i < sequenced_points.size(); i++)
  {
    end_idx = i;
    if( i < sequenced_points.size() - 1)
    {
      const PointXYZ& p_current = sequenced_points[i];
      const PointXYZ& p_next = sequenced_points[i+1];
      Eigen::Vector3d diff = (p_next.getArray3fMap() - p_current.getArray3fMap()).cast<double>();
      if((diff.norm() < split_dist))
      {
        continue;
      }
    }

    if(end_idx == start_idx )
    {
      // single point, discard
      start_idx = i+1;
      continue;
    }

    // save segment
    PointCloud<PointXYZ>::Ptr segment_points = boost::make_shared<PointCloud<PointXYZ>>();
    for(std::size_t p_idx = start_idx ; p_idx <= end_idx; p_idx++)
    {
      auto& p_current = sequenced_points[p_idx];
      if(p_idx > start_idx)
      {
        auto& p_prev = segment_points->back();
        Eigen::Vector3d diff = (p_current.getArray3fMap() - p_prev.getArray3fMap()).cast<double>();
        if(diff.norm() < MIN_POINT_DIST)
        {
          // too close do not add
          continue;
        }
      }
      segment_points->push_back(p_current);
    }

    LOG4CXX_DEBUG(logger_,"Creating sequence [" << start_idx << ",  "<< end_idx<<"] with "
                  << segment_points->size()<<" points");
    if(segment_points->size() <=1)
    {
      LOG4CXX_DEBUG(logger_,"Ignoring segment of 1 point");
      continue;
    }
    sequenced_points_vec.push_back(segment_points);
    start_idx = i+1;
  }

  LOG4CXX_DEBUG(logger_,"Computed " << sequenced_points_vec.size() <<" sequences");
  return sequenced_points_vec;
}

void RegionDetector::findClosedCurves(const std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>& sequenced_curves_vec, double max_dist,
                                 std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>& closed_curves_vec,
                                 std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>& open_curves_vec)
{
  // check if closed, it is assumed that the points have already been sequenced
  for(pcl::PointCloud<pcl::PointXYZ>::Ptr curve_points : sequenced_curves_vec)
  {
    Eigen::Vector3d diff = (curve_points->front().getArray3fMap() - curve_points->back().getArray3fMap()).cast<double>();
    if(diff.norm() < max_dist)
    {
      // copying first point to end of cloud to close the curve
      curve_points->push_back(curve_points->front());

      // saving
      closed_curves_vec.push_back(curve_points);
      LOG4CXX_DEBUG(logger_,"Found closed curve with "<< curve_points->size() << " points");
    }
    else
    {
      open_curves_vec.push_back(curve_points);
      LOG4CXX_DEBUG(logger_,"Found open curve with "<< curve_points->size() << " points");
    }
  }
}

RegionDetector::RegionDetector(log4cxx::LoggerPtr logger):
    logger_(logger ? logger : createDefaultInfoLogger("Default"))
{
  RegionDetectionConfig cfg;
  if(!configure(cfg))
  {
    throw std::runtime_error("Invalid configuration");
  }
}

RegionDetector::~RegionDetector()
{

}

RegionDetector::RegionDetector(const RegionDetectionConfig &config, log4cxx::LoggerPtr logger):
    logger_(logger ? logger : createDefaultInfoLogger("Default"))
{
  if(!configure(config))
  {
    throw std::runtime_error("Invalid configuration");
  }
}

log4cxx::LoggerPtr RegionDetector::createDefaultInfoLogger(const std::string& logger_name)
{
  using namespace log4cxx;
  PatternLayoutPtr pattern_layout(new PatternLayout("[\%-5p] [\%c](L:\%L): \%m\%n"));
  ConsoleAppenderPtr console_appender(new ConsoleAppender(pattern_layout));
  log4cxx::LoggerPtr logger(Logger::getLogger(logger_name));
  logger->addAppender(console_appender);
  logger->setLevel(Level::getInfo());
  return logger;
}

log4cxx::LoggerPtr RegionDetector::createDefaultDebugLogger(const std::string& logger_name)
{
  using namespace log4cxx;
  PatternLayoutPtr pattern_layout(new PatternLayout("[\%-5p] [\%c](L:\%L): \%m\%n"));
  ConsoleAppenderPtr console_appender(new ConsoleAppender(pattern_layout));
  log4cxx::LoggerPtr logger(Logger::getLogger(logger_name));
  logger->addAppender(console_appender);
  logger->setLevel(Level::getDebug());
  return logger;
}

bool RegionDetector::configure(const RegionDetectionConfig &config)
{
  cfg_ = std::make_shared<RegionDetectionConfig>(config);
  return cfg_!=nullptr;
}

bool RegionDetector::configure(const std::string &yaml_str)
{
  return false;
}

log4cxx::LoggerPtr RegionDetector::getLogger()
{
  return logger_;
}

const RegionDetectionConfig& RegionDetector::getConfig()
{
  return *cfg_;
}

void RegionDetector::updateDebugWindow(const cv::Mat& im) const
{
  using namespace cv;
  const RegionDetectionConfig::OpenCVCfg& opencv_cfg = cfg_->opencv_cfg;

  if(!opencv_cfg.debug_mode_enable)
  {
    return;
  }

  // check if window is open
  const std::string wname = opencv_cfg.debug_window_name + std::to_string(window_counter_);
  if(cv::getWindowProperty(wname, cv::WND_PROP_VISIBLE) <= 0)
  {
    // create window then
    namedWindow(wname, WINDOW_AUTOSIZE);
    std::string msg = boost::str(boost::format("Created opencv window \"%s\"") % wname );
    LOG4CXX_DEBUG(logger_,msg);
  }

  imshow( wname, im.clone() );
  cv::waitKey(100);

  if(opencv_cfg.debug_wait_key)
  {
    cv::waitKey();
  }
}

RegionDetector::Result RegionDetector::compute2dContours( cv::Mat input,
                                                           std::vector<std::vector<cv::Point> >& contours_indices,
                                                           cv::Mat& output) const
{
  bool success = false;
  std::string err_msg;
  output = input;
  const RegionDetectionConfig::OpenCVCfg& config = cfg_->opencv_cfg;

  //  ======================== convert to grayscale ========================
  cv::cvtColor( output.clone(), output, cv::COLOR_RGB2GRAY );
  LOG4CXX_ERROR(logger_,"2D analysis: Grayscale Conversion");
  updateDebugWindow(output);

  // ======================== inverting ========================
  if(config.invert_image)
  {
    cv::Mat inverted = cv::Scalar_<uint8_t>(255) - output;
    output = inverted.clone();
    LOG4CXX_ERROR(logger_,"2D analysis: Inversion");
    updateDebugWindow(output);
  }

  //  ======================== dilating ========================
  if(config.dilation.enable)
  {
    // check values
    if(config.dilation.kernel_size <= 0)
    {
      success = false;
      err_msg = "invalid dilation size";
      LOG4CXX_ERROR(logger_,err_msg)
      return Result(success,err_msg);
    }

    // select dilation type
    if(DILATION_TYPES.count(config.dilation.elem) == 0)
    {
      success = false;
      err_msg = "invalid dilation element";
      LOG4CXX_ERROR(logger_,err_msg)
      return Result(success,err_msg);
    }
    int dilation_type = DILATION_TYPES.at(config.dilation.elem);
    cv::Mat element = cv::getStructuringElement( dilation_type,
                         cv::Size( 2*config.dilation.kernel_size + 1, 2*config.dilation.kernel_size+1 ),
                         cv::Point( config.dilation.kernel_size, config.dilation.kernel_size ) );
    cv::dilate( output.clone(), output, element );
    LOG4CXX_ERROR(logger_,"2D analysis: Dilation");
    updateDebugWindow(output);
  }

  //  ======================== Threshold ========================
  if(config.threshold.enable)
  {
    cv::threshold( output.clone(), output, config.threshold.value, config.threshold.MAX_BINARY_VALUE,
                   config.threshold.type );
    LOG4CXX_ERROR(logger_,"2D analysis: threshold with value of "<< config.threshold.value);
    updateDebugWindow(output);
  }

  //  ======================== Canny Edge Detection ========================
  if(config.canny.enable)
  {
    cv::Mat detected_edges;
    int aperture_size = 2 * config.canny.aperture_size + 1;
    aperture_size = aperture_size < 3  ? 3 : aperture_size;
    cv::Canny( output.clone(), detected_edges, config.canny.lower_threshold, config.canny.upper_threshold, aperture_size );
    output = detected_edges.clone();
    LOG4CXX_ERROR(logger_,"2D analysis: Canny");
    updateDebugWindow(output);
  }

  //  ======================== Contour Detection ========================

  std::vector<cv::Vec4i> hierarchy;
  cv::findContours(output.clone(), contours_indices, hierarchy, config.contour.mode, config.contour.method);

  cv::Mat drawing = cv::Mat::zeros( output.size(), CV_8UC3 );
  LOG4CXX_INFO(logger_,"Contour analysis found "<< contours_indices.size() << "contours");
  for( int i = 0; i< contours_indices.size(); i++ )
  {
   cv::Scalar color = cv::Scalar( RANDOM_NUM_GEN.uniform(0, 255), RANDOM_NUM_GEN.uniform(0,255), RANDOM_NUM_GEN.uniform(0,255) );
   double area = cv::contourArea(contours_indices[i]);
   double arc_length = cv::arcLength(contours_indices[i], false);
   cv::drawContours( drawing, contours_indices, i, color, 2, 8, hierarchy, 0, cv::Point() );
   std::string contour_summary = boost::str(boost::format("c[%i]: s: %i, area: %f, arc %f; (p0: %i, pf: %i); h: %i") %
                                    i % contours_indices[i].size() % area % arc_length %
                                    contours_indices[i].front() % contours_indices[i].back() % hierarchy[i]);
   LOG4CXX_INFO(logger_,contour_summary);
   LOG4CXX_ERROR(logger_,"2D analysis: Contour "<< i);
   updateDebugWindow(drawing);
  }

  output = drawing.clone();
  LOG4CXX_DEBUG(logger_,"Completed 2D analysis");
  return Result(true);
}

bool RegionDetector::compute(const std::vector<DataBundle> &input,
                                                      RegionDetector::RegionResults& regions)
{
  using namespace pcl;
  std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> closed_contours_points, open_contours_points;
  pcl::PointCloud<pcl::PointNormal>::Ptr normals = boost::make_shared<pcl::PointCloud<pcl::PointNormal>>();

  Result res;
  window_counter_ = 0;
  for(const DataBundle& data : input)
  {
    window_counter_++;

    // ============================== Open CV =================================== //
    std::vector<std::vector<cv::Point> > contours_indices;
    LOG4CXX_DEBUG(logger_,"Computing 2d contours");
    cv::Mat output;
    res = compute2dContours(data.image, contours_indices, output);
    regions.images.push_back(output);
    if(!res)
    {
      return false;
    }

    // interpolating to fill gaps
    for(std::size_t i = 0; i < contours_indices.size(); i++)
    {
      std::vector<cv::Point> interpolated_indices;
      const std::vector<cv::Point>& indices = contours_indices[i];
      interpolated_indices.push_back(indices.front());
      for(std::size_t j = 1; j < indices.size(); j++)
      {
        const cv::Point& p1 = indices[j-1];
        const cv::Point& p2 = indices[j];

        int x_coord_dist = std::abs(p2.x - p1.x);
        int y_coord_dist = std::abs(p2.y - p1.y);
        int max_coord_dist = x_coord_dist > y_coord_dist ? x_coord_dist : y_coord_dist;
        if(max_coord_dist <= MIN_PIXEL_DISTANCE)
        {
          interpolated_indices.push_back(p2);
          continue;
        }
        int num_elements = max_coord_dist+1;
        std::vector<int> x_coord = linspace<int>(p1.x, p2.x,num_elements);
        std::vector<int> y_coord = linspace<int>(p1.y, p2.y,num_elements);
        cv::Point p;
        for(std::size_t k = 0 ; k < num_elements ;k++)
        {
          std::tie(p.x, p.y) = std::make_tuple(x_coord[k], y_coord[k]);
          interpolated_indices.push_back(p);
        }
      }
      contours_indices[i] = interpolated_indices;
    }

    // ============================== PCL 2D (pixel coordinates z= 0) =================================== //
    // converting to cloud type for further analysis
    std::vector<PointCloud<PointXYZ>> contours_indices_clouds_vec;
    for(std::size_t i = 0; i < contours_indices.size(); i++)
    {
      pcl::PointCloud<pcl::PointXYZ> contour_indices_cloud;
      contour_indices_cloud = convert2DContourToCloud(contours_indices[i]);
      contours_indices_clouds_vec.push_back(contour_indices_cloud);
    }

    // downsampling
    const RegionDetectionConfig::PCL2DCfg& pcl2d_cfg = cfg_->pcl_2d_cfg;
    int search_radius_2d = 4;
    for(std::size_t i = 0; i < contours_indices_clouds_vec.size(); i++)
    {
      if(pcl2d_cfg.downsampling_radius > 0)
      {
        dowsampleCloud(contours_indices_clouds_vec[i],pcl2d_cfg.downsampling_radius);
      }
    }

    // sequence
    for(std::size_t i = 0; i < contours_indices_clouds_vec.size(); i++)
    {
      contours_indices_clouds_vec[i] = sequence(contours_indices_clouds_vec[i].makeShared());
    }

    // split
    std::vector<PointCloud<PointXYZ>::Ptr> contours_indices_cloud_vec;
    for(std::size_t i = 0; i < contours_indices_clouds_vec.size(); i++)
    {
      std::vector<PointCloud<PointXYZ>::Ptr> temp_indices_cloud_vec = split(contours_indices_clouds_vec[i],
                                                                            pcl2d_cfg.split_dist);
      contours_indices_cloud_vec.insert(contours_indices_cloud_vec.end(),temp_indices_cloud_vec.begin(),
                                        temp_indices_cloud_vec.end());
    }

    // find closed curves
    std::vector<PointCloud<PointXYZ>::Ptr> closed_indices_curves_vec, open_indices_curves_vec;
    findClosedCurves(contours_indices_cloud_vec, pcl2d_cfg.closed_curve_max_dist, closed_indices_curves_vec, open_indices_curves_vec);

    // simplification of closed curves
    for(std::size_t i = 0; i < closed_indices_curves_vec.size(); i++)
    {
      int pre_simplified_size = closed_indices_curves_vec[i]->size();
      if(pre_simplified_size < pcl2d_cfg.simplification_min_points)
      {
        continue;
      }
      closed_indices_curves_vec[i] = concaveHullSimplification(closed_indices_curves_vec[i], pcl2d_cfg.simplification_alpha);
      LOG4CXX_DEBUG(logger_,"Concave hull simplified cloud from "<< pre_simplified_size <<
                    " to "<< closed_indices_curves_vec[i]->size());
      *closed_indices_curves_vec[i] = sequence(closed_indices_curves_vec[i]->makeShared());
      closed_indices_curves_vec[i]->push_back(closed_indices_curves_vec[i]->front());
    }

    // combining closed and open back into single vec
    contours_indices_cloud_vec.clear();
    contours_indices_cloud_vec.insert(contours_indices_cloud_vec.end(),
                                      closed_indices_curves_vec.begin(),
                                      closed_indices_curves_vec.end());
    contours_indices_cloud_vec.insert(contours_indices_cloud_vec.end(),
                                      open_indices_curves_vec.begin(),
                                      open_indices_curves_vec.end());

    // converting to cv points
    contours_indices.clear();
    for(auto& cloud :contours_indices_cloud_vec)
    {
      std::vector<cv::Point> temp_indices = convertCloudTo2DContour(*cloud);
      contours_indices.push_back(temp_indices);
    }

    // ============================== PCL 3D (x, y and z coordinates) =================================== //

    // converting input cloud blob into point cloud of specified point type
    pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud = boost::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
    pcl::fromPCLPointCloud2(data.cloud_blob,*input_cloud);

    // transform cloud
    pcl::transformPointCloud(*input_cloud,*input_cloud,data.transform.cast<float>());

    // extract contours 3d points from 2d pixel locations
    std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> contours_points;
    LOG4CXX_DEBUG(logger_,"Extracting contours from 3d data");
    res = extractContoursFromCloud(contours_indices,input_cloud ,contours_points);
    if(!res)
    {
      return false;
    }

    // cleaning data
    for(auto& contour : contours_points)
    {
      // removing nans
      std::vector<int> nan_indices = {};
      LOG4CXX_DEBUG(logger_,"NaN Removal");
      pcl::removeNaNFromPointCloud(*contour, *contour,nan_indices);

      // statistical outlier removal
      if(cfg_->pcl_cfg.stat_removal.enable)
      {
        LOG4CXX_DEBUG(logger_,"Statistical Outlier Removal");
        pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
        sor.setInputCloud (contour->makeShared());
        sor.setMeanK (cfg_->pcl_cfg.stat_removal.kmeans);
        sor.setStddevMulThresh (cfg_->pcl_cfg.stat_removal.stddev);
        sor.filter (*contour);
      }
    }

    LOG4CXX_DEBUG(logger_,"Computing normals");
    std::vector<pcl::PointCloud<pcl::PointNormal>::Ptr> contours_point_normals;
    res = computeNormals(input_cloud,contours_points,contours_point_normals);
    if(!res)
    {
      return false;
    }

    // adding found curves
    if(!closed_indices_curves_vec.empty())
    {
      closed_contours_points.insert(closed_contours_points.end(),
                                    contours_points.begin(),
                                    std::next(contours_points.begin(),closed_indices_curves_vec.size()));
    }

    if(!open_indices_curves_vec.empty())
    {
      open_contours_points.insert(open_contours_points.end(),
                                    std::next(contours_points.begin(),closed_indices_curves_vec.size()),
                                    contours_points.end());
    }

    // adding point normals
    for(auto& cn : contours_point_normals)
    {
      (*normals) += *cn;
    }
  }

  // combining open curves to form closed ones
  std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> closed_curves_points, open_curves_points;
  LOG4CXX_DEBUG(logger_,"Computing closed contours from "<< open_contours_points.size()<<" open curves");
  res = combineIntoClosedRegions(open_contours_points, closed_curves_points, open_curves_points);

  // adding to existing vector of closed and open curves
  closed_contours_points.insert(closed_contours_points.end(),closed_curves_points.begin(),
                                closed_curves_points.end());
  open_contours_points = open_curves_points;

  // simplifying by length
  closed_contours_points = simplifyByMinimunLength(closed_contours_points, cfg_->pcl_cfg.simplification_min_dist);
  open_contours_points = simplifyByMinimunLength(open_contours_points, cfg_->pcl_cfg.simplification_min_dist);

  // filter out those with too few points
  closed_contours_points.erase(std::remove_if(closed_contours_points.begin(),closed_contours_points.end(),
                                              [this](pcl::PointCloud<pcl::PointXYZ>::Ptr& c){
    return c->size() < cfg_->pcl_cfg.min_num_points;
  }), closed_contours_points.end());

  open_contours_points.erase(std::remove_if(open_contours_points.begin(),open_contours_points.end(),
                                              [this](pcl::PointCloud<pcl::PointXYZ>::Ptr& c){
    return c->size() < cfg_->pcl_cfg.min_num_points;
  }), open_contours_points.end());

  LOG4CXX_DEBUG(logger_,"Computing curves normals");
  computePoses(normals,open_contours_points,regions.open_regions_poses);
  computePoses(normals,closed_contours_points,regions.closed_regions_poses);

  std::string msg = boost::str(boost::format("Found %i closed regions and %i open regions") % regions.closed_regions_poses.size() %
                               regions.open_regions_poses.size() );
  if(!regions.closed_regions_poses.empty())
  {
    LOG4CXX_ERROR(logger_, msg);
  }
  else
  {
    LOG4CXX_INFO(logger_, msg);
  }
  return !regions.closed_regions_poses.empty();
}

std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> RegionDetector::simplifyByMinimunLength(const std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>&
                                                                                    segments, double min_length)
{
  using namespace pcl;
  std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> simplified_segments;
  for(const auto& segment: segments)
  {
    PointCloud<PointXYZ>::Ptr simplified_segment = boost::make_shared<PointCloud<PointXYZ>>();
    simplified_segment->push_back(segment->front());
    for(std::size_t i = 1; i < segment->size() -1 ; i++)
    {
      const auto& p0 = simplified_segment->back();
      const auto& p1 = segment->at(i);
      Eigen::Vector3f diff = (p0.getArray3fMap() - p1.getArray3fMap());
      if(diff.norm() > min_length)
      {
        simplified_segment->push_back(p1);
      }
    }
    simplified_segment->push_back(segment->back());

    simplified_segments.push_back(simplified_segment);
  }
  return simplified_segments;
}

RegionDetector::Result RegionDetector::extractContoursFromCloud(
    const std::vector<std::vector<cv::Point> >& contour_indices, pcl::PointCloud<pcl::PointXYZ>::ConstPtr input,
    std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>& contours_points)
{
  // check for organized point clouds
  if(!input->isOrganized())
  {
    std::string err_msg = "Point Cloud not organized";
    LOG4CXX_ERROR(logger_,err_msg)
    return Result(false,err_msg);
  }

  pcl::PointCloud<pcl::PointXYZ> temp_contour_points;
  for(const std::vector<cv::Point>& indices : contour_indices)
  {
    if(indices.empty())
    {
      std::string err_msg = "Empty indices vector was passed";
      LOG4CXX_ERROR(logger_,err_msg)
      return Result(false,err_msg);
    }

    temp_contour_points.clear();
    for(const cv::Point& idx : indices)
    {
      if(idx.x >= input->width || idx.y >= input->height)
      {
        std::string err_msg = "2D indices exceed point cloud size";
        LOG4CXX_ERROR(logger_,err_msg)
        return Result(false,err_msg);

      }
      temp_contour_points.push_back(input->at(idx.x,idx.y));
    }
    contours_points.push_back(boost::make_shared<pcl::PointCloud<pcl::PointXYZ>>(temp_contour_points));
  }

  return Result(!contours_points.empty(),"");
}

RegionDetector::Result RegionDetector::combineIntoClosedRegions(
    const std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>& contours_points,
    std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>& closed_curves,
    std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>& open_curves)
{
  using namespace pcl;

  std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> output_contours_points;
  output_contours_points.assign(contours_points.begin(),contours_points.end());

  std::vector<int> merged_curves_indices;
  // find closed curves
  for(std::size_t i = 0; i < output_contours_points.size(); i++)
  {
    if(std::find(merged_curves_indices.begin(), merged_curves_indices.end(), i) != merged_curves_indices.end())
    {
      // already merged
      LOG4CXX_DEBUG(logger_,"Curve "<< i <<" has already been merged");
      continue;
    }

    // get curve
    PointCloud<PointXYZ>::Ptr curve_points = output_contours_points[i]->makeShared();
    LOG4CXX_DEBUG(logger_,"Attempting to merge Curve "<< i <<" with "<< curve_points->size() <<" points");

    // create merge candidate index list
    std::vector<int> merge_candidate_indices;
    merge_candidate_indices.resize(output_contours_points.size());
    std::iota(merge_candidate_indices.begin(), merge_candidate_indices.end(), 0);

    // remove current curve
    merge_candidate_indices.erase(merge_candidate_indices.begin() + i);

    // search for close curves to merge with
    bool merged_curves = false;
    do
    {
      merged_curves = false;

      // merge with other unmerged curves
      for(int idx : merge_candidate_indices)
      {
        if(std::find(merged_curves_indices.begin(), merged_curves_indices.end(), idx) != merged_curves_indices.end())
        {
          // already merged
          LOG4CXX_DEBUG(logger_,"\tCurve "<< idx <<" has already been merged");
          continue;
        }

        PointCloud<PointXYZ>::Ptr merged_points = boost::make_shared<PointCloud<PointXYZ>>();
        PointCloud<PointXYZ>::Ptr next_curve_points = output_contours_points[idx];
        if(mergeCurves(*curve_points, *next_curve_points,*merged_points))
        {
          *curve_points = *merged_points;
          merged_curves_indices.push_back(i);
          merged_curves_indices.push_back(idx);
          merged_curves = true;
          LOG4CXX_DEBUG(logger_,"Merged Curve "<< idx <<" with "<< next_curve_points->size() << " points to curve "<< i <<
              ", final curve has "<< curve_points->size() << " points");
        }

        // removing repeated
        std::sort(merged_curves_indices.begin(), merged_curves_indices.end());
        auto it = std::unique(merged_curves_indices.begin(), merged_curves_indices.end());
        merged_curves_indices.resize( std::distance(merged_curves_indices.begin(),it) );
      }

    }while(merged_curves);

    // check if closed
    Eigen::Vector3d diff = (curve_points->front().getArray3fMap() - curve_points->back().getArray3fMap()).cast<double>();
    if(diff.norm() < cfg_->pcl_cfg.closed_curve_max_dist)
    {
      // copying first point to end of cloud to close the curve
      curve_points->push_back(curve_points->front());

      // saving
      closed_curves.push_back(curve_points);
      LOG4CXX_DEBUG(logger_,"Found closed curve with "<< curve_points->size() << " points");
    }
    else
    {
      open_curves.push_back(curve_points);
      LOG4CXX_DEBUG(logger_,"Copied curve "<< i << " into open curves vector");
    }

    merged_curves_indices.push_back(i);
  }

  // copying open curves that were not merged
  for(std::size_t i =0; i < output_contours_points.size(); i++)
  {
    if(std::find(merged_curves_indices.begin(),merged_curves_indices.end(), i) != merged_curves_indices.end())
    {
      continue;
    }
    open_curves.push_back(output_contours_points[i]);
    LOG4CXX_DEBUG(logger_,"Copied unmerged curve "<< i << " into open curves vector");
  }

  if(closed_curves.empty())
  {
    std::string err_msg = "Found no closed curves";
    LOG4CXX_ERROR(logger_,err_msg)
    return Result(false,err_msg);
  }
  LOG4CXX_INFO(logger_,"Found " << closed_curves.size() <<  " closed curves");
  return true;
}

RegionDetector::Result RegionDetector::mergeCurves(pcl::PointCloud<pcl::PointXYZ> c1, pcl::PointCloud<pcl::PointXYZ> c2,
                                 pcl::PointCloud<pcl::PointXYZ>& merged)
{
  std::vector<double> end_points_distances(4);

  // compute end point distances
  Eigen::Vector3d dist;
  dist = (c1.front().getArray3fMap() - c2.front().getArray3fMap()).cast<double>();
  end_points_distances[0] = dist.norm();

  dist = (c1.front().getArray3fMap() - c2.back().getArray3fMap()).cast<double>();
  end_points_distances[1] = dist.norm();

  dist = (c1.back().getArray3fMap() - c2.front().getArray3fMap()).cast<double>();
  end_points_distances[2] = dist.norm();

  dist = (c1.back().getArray3fMap() - c2.back().getArray3fMap()).cast<double>();
  end_points_distances[3] = dist.norm();

  std::vector<double>::iterator min_pos = std::min_element(end_points_distances.begin(), end_points_distances.end());
  if(*min_pos > cfg_->pcl_cfg.max_merge_dist)
  {
    // curves are too far, not merging
    return false;
  }

  std::size_t merge_method = std::distance(end_points_distances.begin(), min_pos);
  switch(merge_method)
  {
    case 0: // front2 to front1
      std::reverse(c2.begin(), c2.end());
      c1.insert(c1.begin(),c2.begin(), c2.end());
      break;

    case 1: // back2 to front1
      c1.insert(c1.begin(),c2.begin(), c2.end());
      break;

    case 2: // back1 to front2
      c1.insert(c1.end(),c2.begin(), c2.end());
      break;

    case 3: // back1 to back2
      std::reverse(c2.begin(), c2.end());
      c1.insert(c1.end(),c2.begin(), c2.end());
      break;
  }

  //merged = sequence(c1.makeShared());
  merged = c1;
  return true;
}

RegionDetector::Result RegionDetector::computeNormals(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr source_cloud,
                                      const std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> &curves_points,
                                      std::vector<pcl::PointCloud<pcl::PointNormal>::Ptr> &curves_normals)
{

  const config_3d::NormalEstimationCfg& cfg = cfg_->pcl_cfg.normal_est;

  // downsample first
  pcl::PointCloud<pcl::PointXYZ>::Ptr source_cloud_downsampled = source_cloud->makeShared();
  dowsampleCloud(*source_cloud_downsampled,cfg.downsampling_radius);

  // first compute normals
  pcl::PointCloud<pcl::PointNormal>::Ptr source_cloud_normals (new pcl::PointCloud<pcl::PointNormal>);
  pcl::NormalEstimation<pcl::PointXYZ, pcl::PointNormal> ne;
  ne.setInputCloud(source_cloud_downsampled);
  ne.setViewPoint(cfg.viewpoint_xyz[0], cfg.viewpoint_xyz[1], cfg.viewpoint_xyz[2]);
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ> ());
  ne.setSearchMethod (tree);
  ne.setRadiusSearch (cfg.search_radius);
  ne.compute (*source_cloud_normals);

  // create kdtree to search cloud with normals
  pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
  kdtree.setEpsilon(cfg.kdtree_epsilon);
  kdtree.setInputCloud(source_cloud_downsampled);


  const int MAX_NUM_POINTS = 1;
  std::vector<int> nearest_indices(MAX_NUM_POINTS);
  std::vector<float> nearest_distances(MAX_NUM_POINTS);
  for(auto& curve : curves_points)
  {
    // search point and copy its normal
    pcl::PointCloud<pcl::PointNormal>::Ptr curve_normals = boost::make_shared<pcl::PointCloud<pcl::PointNormal>>();
    curve_normals->reserve(curve->size());
    for(auto& search_p : *curve)
    {
      int nearest_found = kdtree.nearestKSearch(search_p,MAX_NUM_POINTS, nearest_indices, nearest_distances);
      if(nearest_found <=0 )
      {
        std::string err_msg = "Found no points near curve, can not get normal vector";
        LOG4CXX_ERROR(logger_,err_msg)
        return Result(false,err_msg);
      }
      pcl::PointNormal pn;
      pcl::copyPoint(source_cloud_normals->at(nearest_indices.front()), pn);
      pcl::copyPoint(search_p, pn);
      curve_normals->push_back(pn);
    }
    curves_normals.push_back(curve_normals);
  }
  return true;
}

RegionDetector::Result RegionDetector::computePoses(pcl::PointCloud<pcl::PointNormal>::ConstPtr source_normal_cloud,
                                                      std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> &curves_points,
                                                      std::vector<EigenPose3dVector>& curves_poses)
{
  using namespace Eigen;
  const config_3d::NormalEstimationCfg& cfg = cfg_->pcl_cfg.normal_est;

  // create kdtree to search cloud with normals
  pcl::PointCloud<pcl::PointXYZ>::Ptr source_points = boost::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  pcl::copyPointCloud(*source_normal_cloud, *source_points);
  pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
  kdtree.setEpsilon(cfg.kdtree_epsilon);
  kdtree.setInputCloud(source_points);

  const unsigned int MAX_NUM_POINTS = 1;
  std::vector<int> nearest_indices(MAX_NUM_POINTS);
  std::vector<float> nearest_distances(MAX_NUM_POINTS);
  int curve_idx = -1;
  for(auto& curve : curves_points)
  {
    curve_idx++;

    // search point and copy its normal
    pcl::PointCloud<pcl::Normal>::Ptr curve_normals = boost::make_shared<pcl::PointCloud<pcl::Normal>>();
    curve_normals->reserve(curve->size());
    for(auto& search_p : *curve)
    {
      int nearest_found = kdtree.nearestKSearch(search_p,MAX_NUM_POINTS,nearest_indices,nearest_distances);
      if(nearest_found <=0 )
      {
        std::string err_msg = boost::str(
            boost::format("Kdtree found no nearby points during pose computation"));
        LOG4CXX_ERROR(logger_,err_msg);
        return Result(false,err_msg);
      }
      pcl::Normal p;
      pcl::copyPoint(source_normal_cloud->at(nearest_indices.front()), p);
      curve_normals->push_back(p);
    }

    LOG4CXX_DEBUG(logger_,"Computing pose orientation vectors for curve "<< curve_idx << " with "<< curve->size() <<" points");
    EigenPose3dVector curve_poses;
    pcl::PointNormal p1, p2;
    Isometry3d pose;
    Vector3d x_dir, z_dir, y_dir;
    double dir = 1.0;
    for(std::size_t i = 0; i < curve->size(); i++)
    {
      std::size_t idx_current = i;
      std::size_t idx_next = i+1;
      dir = 1.0;

      if(idx_next >= curve->size())
      {
        idx_current = i;
        idx_next = i-1;
        dir = -1.0;
      }

      pcl::copyPoint(curve->at(idx_current),p1);
      pcl::copyPoint(curve_normals->at(idx_current),p1);
      pcl::copyPoint(curve->at(idx_next),p2);
      pcl::copyPoint(curve_normals->at(idx_next),p2);

      x_dir = dir * (p2.getVector3fMap() - p1.getVector3fMap()).normalized().cast<double>();
      z_dir = Vector3d(p1.normal_x, p1.normal_y, p1.normal_z).normalized();
      y_dir = z_dir.cross(x_dir).normalized();
      z_dir = x_dir.cross(y_dir).normalized();

      pose = Translation3d(p1.getVector3fMap().cast<double>());
      pose.matrix().block<3,3>(0,0) = toRotationMatrix(x_dir, y_dir, z_dir);
      curve_poses.push_back(pose);
    }
    curves_poses.push_back(curve_poses);
  }

  return true;
}

} /* namespace region_detection_core */
