#include <ros/package.h>
#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/subscriber.h>
#include <cv_bridge/cv_bridge.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/transforms.h>
#include <tr1/memory>
#include "cv.h"
#include <sstream>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/norms.h>
#include <pcl/features/normal_3d.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/ModelCoefficients.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/visualization/common/common.h>

#include <pcl_ros/transforms.h>
#include <pcl/io/pcd_io.h>
#include <pcl/visualization/pcl_visualizer.h>

#include <sys/types.h>
#include <boost/filesystem.hpp>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <string.h>
#include "std_msgs/String.h"

// ROS
#include <sensor_msgs/PointCloud2.h>
#include <std_msgs/String.h>

#include <object_detection/DetectObject.h>
#include <pose_estimation/PoseEstimation.h>

typedef pose_estimation::PoseEstimation PoseEstimation;
typedef object_detection::DetectObject MsgT;

using namespace std;
using namespace tr1;
using namespace message_filters;
using namespace sensor_msgs;
using namespace tf;
using namespace pcl_ros;

typedef pcl::PointXYZRGBA PointT;
typedef pcl::PointXYZRGB PointA;
typedef pcl::PointCloud<PointT> CloudT;



MsgT msgglobal;
Eigen::Matrix4f keep_latest_best_pose;

PoseEstimation poseEstimationMsgT;

ros::Publisher publish_for_vizualizer;

vector<image_transport::SubscriberFilter *> subscribterVector;

vector<ros::Subscriber*> point_cloud_vector;
vector<ros::Subscriber*> stereo_point_cloud_vector;

image_transport::SubscriberFilter* sub_temp_1;
image_transport::SubscriberFilter* sub_temp_2;

vector<message_filters::TimeSynchronizer<Image, Image> *> timeSync;

typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image> SyncPolicyT;
vector<message_filters::Synchronizer<SyncPolicyT> *> timeSyncApprox;

pcl::PointCloud<PointT> stereo_pointCloud;
pcl::PointCloud<PointT> carmine_pointCloud;

//for constrains
std::vector<double> constr_conveyor, constr_table;


//ros::ServiceClient pose_estimation_service_client;
ros::ServiceClient getPose;
string object_path;

void stereoPointCloudSaver(ros::NodeHandle nh, string name);

void stereoPointCloudCallback(const sensor_msgs::PointCloud2ConstPtr& PointCloudROS){
    pcl::fromROSMsg<PointT>(*PointCloudROS, stereo_pointCloud);
    //std::cout << "stereo_pointCloud.size(): " << stereo_pointCloud.size() << std::endl;
}

void kinectPointCloudSaver(ros::NodeHandle, string name);

void kinectPointCloudCallback(const sensor_msgs::PointCloud2ConstPtr& PointCloudROS){
    pcl::fromROSMsg<PointT>(*PointCloudROS, carmine_pointCloud);
}

void stereoPointCloudSaver(ros::NodeHandle nh, string name){

    std::stringstream PointCloudPath;
    PointCloudPath << name << "/points";

    stereo_point_cloud_vector.push_back(new ros::Subscriber());
    *stereo_point_cloud_vector.back() = nh.subscribe (PointCloudPath.str(), 1, stereoPointCloudCallback);
}

void kinectPointCloudSaver(ros::NodeHandle nh, string name){
    std::stringstream PointCloudPath;
    PointCloudPath << name << "/depth_registered/points";


    point_cloud_vector.push_back(new ros::Subscriber());
    *point_cloud_vector.back() = nh.subscribe (PointCloudPath.str(), 1, kinectPointCloudCallback);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////
pcl::PointCloud<PointT> getCutRegion(pcl::PointCloud<PointA>::Ptr object_transformed, float _cut_x, float _cut_y, float _cut_z, pcl::PointCloud<PointT> scene){
    //----------------
    Eigen::Vector4f centroid;
    pcl::compute3DCentroid(*object_transformed, centroid);
    Eigen::Matrix3f covariance;
    computeCovarianceMatrixNormalized(*object_transformed, centroid, covariance);
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> eigen_solver(covariance, Eigen::ComputeEigenvectors);
    Eigen::Matrix3f eigDx = eigen_solver.eigenvectors();
    eigDx.col(2) = eigDx.col(0).cross(eigDx.col(1));

    // move the points to the that reference frame
    Eigen::Matrix4f p2w(Eigen::Matrix4f::Identity());
    p2w.block<3,3>(0,0) = eigDx.transpose();
    p2w.block<3,1>(0,3) = -1.f * (p2w.block<3,3>(0,0) * centroid.head<3>());
    pcl::PointCloud<PointA> cPoints;
    pcl::transformPointCloud(*object_transformed, cPoints, p2w);
    PointA min_pt, max_pt;
    pcl::getMinMax3D(cPoints, min_pt, max_pt);
    const Eigen::Vector3f mean_diag = 0.5f*(max_pt.getVector3fMap() + min_pt.getVector3fMap());
    // final transform

    const Eigen::Quaternionf qfinal(eigDx);
    const Eigen::Vector3f tfinal = eigDx*mean_diag + centroid.head<3>();
    pcl::PointXYZRGB minp, maxp;
    Eigen::Matrix4f _tr = Eigen::Matrix4f::Identity();
    _tr.topLeftCorner<3,3>() = qfinal.toRotationMatrix();
    _tr.block<3,1>(0,3) = tfinal;

    float _x = (max_pt.x-min_pt.x)* _cut_x;
    float _y = (max_pt.y-min_pt.y) * _cut_y;
    float _z = (max_pt.z-min_pt.z) * _cut_z;

    //****
    _tr = _tr.inverse().eval();

    pcl::PointCloud<PointT>::Ptr cloud_bm (new pcl::PointCloud<PointT>);
    *cloud_bm = scene;

    pcl::PointIndices::Ptr object_indices (new pcl::PointIndices);
    for (size_t i = 0; i < cloud_bm->size(); i++){
        PointT p = (*cloud_bm)[i];

        p.getVector4fMap() = _tr * p.getVector4fMap();
        if(fabsf(p.x) <= _x && fabsf(p.y) <= _y && fabsf(p.z) <= _z ) {
            object_indices->indices.push_back(i);
        }
    }
    pcl::PointCloud<PointT> small_cube;

    small_cube.height = 1;
    small_cube.width = object_indices->indices.size();
    small_cube.points.resize(small_cube.height * small_cube.width);

    pcl::copyPointCloud (*cloud_bm , object_indices->indices, small_cube );
    if (small_cube.size() > 0) pcl::io::savePCDFile("small_cube01.pcd", small_cube);


    return small_cube;
}
//----------------------------------------
////////////////////////////////////////////////////////////////////////////////////////////////////////////
pcl::PointCloud<PointT> getCutRegionForTable(pcl::PointCloud<PointA>::Ptr object_transformed, float _cut_x, float _cut_y, float _cut_z, pcl::PointCloud<PointT> scene){
    //----------------
    Eigen::Vector4f centroid;
    pcl::compute3DCentroid(*object_transformed, centroid);
    Eigen::Matrix3f covariance;
    computeCovarianceMatrixNormalized(*object_transformed, centroid, covariance);
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> eigen_solver(covariance, Eigen::ComputeEigenvectors);
    Eigen::Matrix3f eigDx = eigen_solver.eigenvectors();
    eigDx.col(2) = eigDx.col(0).cross(eigDx.col(1));

    // move the points to the that reference frame
    Eigen::Matrix4f p2w(Eigen::Matrix4f::Identity());
    p2w.block<3,3>(0,0) = eigDx.transpose();
    p2w.block<3,1>(0,3) = -1.f * (p2w.block<3,3>(0,0) * centroid.head<3>());
    pcl::PointCloud<PointA> cPoints;
    pcl::transformPointCloud(*object_transformed, cPoints, p2w);
    PointA min_pt, max_pt;
    pcl::getMinMax3D(cPoints, min_pt, max_pt);
    const Eigen::Vector3f mean_diag = 0.5f*(max_pt.getVector3fMap() + min_pt.getVector3fMap());
    // final transform

    const Eigen::Quaternionf qfinal(eigDx);
    const Eigen::Vector3f tfinal = eigDx*mean_diag + centroid.head<3>();
    pcl::PointXYZRGB minp, maxp;
    Eigen::Matrix4f _tr = Eigen::Matrix4f::Identity();
    _tr.topLeftCorner<3,3>() = qfinal.toRotationMatrix();
    _tr.block<3,1>(0,3) = tfinal;

    float _x = (max_pt.x-min_pt.x)* _cut_x;
    float _y = (max_pt.y-min_pt.y) * _cut_y;
    float _z = (max_pt.z-min_pt.z) * _cut_z;

    //****
    _tr = _tr.inverse().eval();

    pcl::PointCloud<PointT>::Ptr cloud_bm (new pcl::PointCloud<PointT>);
    *cloud_bm = scene;

    pcl::PointIndices::Ptr object_indices (new pcl::PointIndices);
    for (size_t i = 0; i < cloud_bm->size(); i++){
        PointT p = (*cloud_bm)[i];

        p.getVector4fMap() = _tr * p.getVector4fMap();
        if (p.x < (min_pt.x * 1.8) || p.y < (min_pt.y * 1.8) || p.z < (min_pt.z * 1.8)
                 || p.x > max_pt.x * 3|| p.y > max_pt.y* 1 || p.z > max_pt.z * 1 )
                {

                }else object_indices->indices.push_back(i);

    }
    pcl::PointCloud<PointT> small_cube;

    small_cube.height = 1;
    small_cube.width = object_indices->indices.size();
    small_cube.points.resize(small_cube.height * small_cube.width);

    pcl::copyPointCloud (*cloud_bm , object_indices->indices, small_cube );
    if (small_cube.size() > 0) pcl::io::savePCDFile("small_cube01.pcd", small_cube);


    return small_cube;
}
//------------------------------------------------------------------------------
pcl::PointCloud<PointT> cutTable(pcl::PointCloud<PointA>::Ptr object, pcl::PointCloud<PointT> scene, MsgT msgglobal1){

    tf::Transform transform;
    tf::transformMsgToTF(msgglobal1.response.poses[0], transform);

    Eigen::Matrix4f m_init, m;
    transformAsMatrix(transform, m_init);
    m_init(12) = m_init(12)/1000;
    m_init(13) = m_init(13)/1000;
    m_init(14) = m_init(14)/1000;
    m = m_init;

    m(0) = 0.999995;    m(1) = -0.00224549;    m(2) = 0.00208427;
    m(4) = 0.00224671;    m(5) = 0.999997;    m(6) = -0.000595095;
    m(8) = -0.00208288;    m(9) = 0.000599744;    m(10) = 0.999998;
    m(12) = 7.07721/1000;    m(13) = -0.0677484/1000;    m(14) = -0.741719/1000;
    m(3) = 0;    m(7) = 0;    m(11) = 0;  m(15) = 1;

    pcl::PointCloud<PointA>::Ptr object_transformed (new pcl::PointCloud<PointA>);
    pcl::PointCloud<PointA>::Ptr table (new pcl::PointCloud<PointA>);
    table->height = 1;
    table->width = 8;
    table->points.resize(table->height * table->width);
    
    (*table)[0] = (*object)[108000]; (*table)[1] = (*object)[108001];
    (*table)[2] = (*object)[108002]; (*table)[3] = (*object)[108003];
    (*table)[4] = (*object)[108004]; (*table)[5] = (*object)[108005];
    (*table)[6] = (*object)[108006]; (*table)[7] = (*object)[108007];
    

    pcl::transformPointCloud(*table, *object_transformed, m);
    pcl::PointCloud<PointT> small_cube = getCutRegionForTable(object_transformed, 0.9, 0.9, 0.9, scene);
    return small_cube;
}
//------------------------------------------------------------------

pcl::PointCloud<PointT> cutConveyourBelt(pcl::PointCloud<PointA>::Ptr object, pcl::PointCloud<PointT> scene, Eigen::Matrix4f m){//MsgT msgglobal1){

    /*tf::Transform transform;
    tf::transformMsgToTF(msgglobal1.response.poses[0], transform);

    Eigen::Matrix4f m_init, m;
    transformAsMatrix(transform, m_init);
    m_init(12) = m_init(12)/1000;
    m_init(13) = m_init(13)/1000;
    m_init(14) = m_init(14)/1000;
    m = m_init; */


    pcl::PointCloud<PointA>::Ptr object_transformed (new pcl::PointCloud<PointA>);
    pcl::PointCloud<PointA>::Ptr conveyour (new pcl::PointCloud<PointA>);
    conveyour->height = 1;
    conveyour->width = 8;
    conveyour->points.resize(conveyour->height * conveyour->width);

    (*conveyour)[0] = (*object)[5892]; (*conveyour)[1] = (*object)[1848];
    (*conveyour)[2] = (*object)[35255]; (*conveyour)[3] = (*object)[41087];
    (*conveyour)[4] = (*object)[45012]; (*conveyour)[5] = (*object)[45013];
    (*conveyour)[6] = (*object)[45014]; (*conveyour)[7] = (*object)[45015];

    pcl::transformPointCloud(*conveyour, *object_transformed, m);
    pcl::PointCloud<PointT> small_cube = getCutRegion(object_transformed, 0.8, 0.8, 0.5, scene);
    return small_cube;
}
//--------------------------------------------------------------------------------------------
void storeResults(PoseEstimation::Response &resp, MsgT data, sensor_msgs::PointCloud2 scenei, pcl::PointCloud<PointT> object){
    // Store results
    resp.labels_int.clear();
    resp.poses.clear();
    resp.pose_value.clear();

    resp.labels_int.resize(data.response.poses.size());
    resp.poses.reserve(data.response.poses.size());
    resp.pose_value.resize(data.response.poses.size());

    sensor_msgs::PointCloud2 objecti;
    pcl::toROSMsg(object, objecti);

    resp.scene = scenei;
    resp.object = objecti;

    resp.poses = data.response.poses;
    resp.pose_value = data.response.pose_value;
    resp.labels_int = data.response.labels_int;

    //publish here
    publish_for_vizualizer.publish(resp);
}
//-------------------------------------------------------------------------------------------------
bool detectRotorcaps(pcl::PointCloud<PointT> cutScene, PoseEstimation::Response &resp, std::vector<double> constr, bool viz){

    ros::NodeHandle nh("~");
    std::string rotorcapPCD;
    nh.getParam("rotorcapPCD", rotorcapPCD);

    // get properties on gripper name and on grasp database directory

    pcl::console::print_value(" Starting rotorcap detection! \n");

    sensor_msgs::PointCloud2 scenei;
    pcl::toROSMsg(cutScene, scenei);

    MsgT msgrotorcaps;
    msgrotorcaps.request.visualize = viz;
    msgrotorcaps.request.rotorcap = true;
    msgrotorcaps.request.table = false;
    msgrotorcaps.request.threshold = 5;
    msgrotorcaps.request.cothres = 1;
    msgrotorcaps.request.objects.clear();
    msgrotorcaps.request.objects.push_back(rotorcapPCD);

    msgrotorcaps.request.constrains = constr;

    pcl::PointCloud<PointT> object;
    pcl::io::loadPCDFile(rotorcapPCD, object);

    msgrotorcaps.request.cloud = scenei;
    if(getPose.call(msgrotorcaps)) {
	 storeResults(resp, msgrotorcaps, scenei, object);
	return true;
    } else return false;

}
//--------------------------------------------------------------------------------------------
bool detectConveyourBeltAndRotorcaps(PoseEstimation::Response &resp, bool viz, Eigen::Matrix4f& m){
    //---POSE ESTIMATION BLOCK
    ROS_INFO("Subscribing to /service/getPose ...");
    ros::service::waitForService("/service/getPose");

    sensor_msgs::PointCloud2 scenei;
    pcl::toROSMsg(carmine_pointCloud, scenei);
    MsgT msgconveyor;
    msgconveyor.request.visualize = viz;
    msgconveyor.request.table = false;
    msgconveyor.request.threshold = 10;
    msgconveyor.request.cothres = 1;

    msgconveyor.request.objects.clear();
    msgconveyor.request.objects.push_back(object_path);

    msgconveyor.request.cloud = scenei;
    pcl::PointCloud<pcl::PointXYZRGBA> outSmall;
    
    pcl::console::print_warn("Trying to detect conveyor!\n");
    if (getPose.call(msgconveyor)){
	
	tf::Transform transform;
    	tf::transformMsgToTF(msgconveyor.response.poses[0], transform);
        Eigen::Matrix4f m_init;
        transformAsMatrix(transform, m_init);
        m_init(12) = m_init(12)/1000;
        m_init(13) = m_init(13)/1000;
        m_init(14) = m_init(14)/1000;
        m = m_init; 

   /* m(0) = 0.999403;    m(1) = 0.0246888;    m(2) = 0.00819257;
    m(4) = -0.024643;    m(5) = 0.99968;    m(6) = -0.00564397;
    m(8) = -0.0083293;    m(9) = 0.00544018;    m(10) = 0.999951;
    m(12) = -0.0068171;    m(13) = 0.0200884;    m(14) = -0.0348979;
    m(3) = 0;    m(7) = 0;    m(11) = 0;  m(15) = 1; */

	keep_latest_best_pose = m;		
	return true;
    } else {
	return false;
       	//ROS_ERROR("Something went wrong when calling /object_detection/global");
    }            
}
//----------------------------
void detectTableAndRotorcaps(PoseEstimation::Response &resp, bool viz){
    //---POSE ESTIMATION BLOCK
    ROS_INFO("Subscribing to /service/getPose ...");
    ros::service::waitForService("/service/getPose");

    sensor_msgs::PointCloud2 scenei;
    pcl::toROSMsg(carmine_pointCloud, scenei);

    msgglobal.request.visualize = viz;
    msgglobal.request.table = false;
    msgglobal.request.threshold = 20;
    msgglobal.request.cothres = 1;

    msgglobal.request.objects.clear();
    msgglobal.request.objects.push_back(object_path);

    msgglobal.request.cloud = scenei;
    if(getPose.call(msgglobal)) {
        pcl::PointCloud<PointA>::Ptr object(new pcl::PointCloud<PointA>());
        pcl::io::loadPCDFile(object_path, *object);

        if (stereo_pointCloud.size() > 1){
	   pcl::PointCloud<pcl::PointXYZRGBA> outSmall = cutTable(object, stereo_pointCloud, msgglobal);
            if (outSmall.size() > 0) detectRotorcaps(outSmall, resp, constr_table, false);
	    else detectTableAndRotorcaps(resp, viz);
        }
    } else {
        ROS_ERROR("Something went wrong when calling /object_detection/global");
    }
}

void saveLocallyPointClouds(std::string pcddir){
	pcl::io::savePCDFile(pcddir+"carmine_PC.pcd", carmine_pointCloud);
        pcl::io::savePCDFile(pcddir+"carmine_PC.pcd", carmine_pointCloud);
        pcl::io::savePCDFileBinary(pcddir+"stereo_PC_binary.pcd", stereo_pointCloud);
        pcl::io::savePCDFile(pcddir+"stereo_PC.pcd", stereo_pointCloud);
}

//------------------------------------------------------------------------------------------------------------------------
bool pose_estimation_service(PoseEstimation::Request &req, PoseEstimation::Response &resp){
    ROS_INFO("Starting pose estimation service\n!");
    std::string scenario = req.scenario;

    if(scenario == "save_point_clouds"){
        ros::NodeHandle nh("~");
        std::string pcddir;
            nh.getParam("pcddir", pcddir);

        if ((stereo_pointCloud.size() < 0 && carmine_pointCloud.size() < 0)){
            pcl::console::print_warn("Waiting for point clouds!\n");
        }
        else {
            if (carmine_pointCloud.size() > 0) pcl::io::savePCDFile(pcddir+"carmine_PC.pcd", carmine_pointCloud);
            if (stereo_pointCloud.size() > 0) pcl::io::savePCDFile(pcddir+"stereo_PC.pcd", stereo_pointCloud);
            pcl::console::print_value("Saving stereo and carmine point clouds\n");
        }
    }
    else if(scenario == "detect_rotorcaps_on_table"){
        ros::NodeHandle nh("~");
        std::string pcddir;
        nh.getParam("table_2PCD", object_path);
        nh.getParam("pcddir", pcddir);

        if (carmine_pointCloud.size()> 0 && stereo_pointCloud.size() > 0) {
	    pcl::console::print_value("Detecting table, then rotorcaps\n");
            saveLocallyPointClouds(pcddir);
            detectTableAndRotorcaps(resp, false);
        }
        else pcl::console::print_error("Cannot grasp frame from carmine & stereo! Are you sure they are running?!");
    }
///------------------------------------------------------------------------------
    else if (scenario == "detect_rotorcaps_on_coveyour_belt"){
        ros::NodeHandle nh("~");
        std::string pcddir;
        nh.getParam("conveyor_belt_2PCD", object_path);
        nh.getParam("pcddir", pcddir);

        if (stereo_pointCloud.size() > 0 && carmine_pointCloud.size() > 0) {
	    pcl::console::print_value("Detecting conveyor, then rotorcaps\n");
	    saveLocallyPointClouds(pcddir);
            
	    Eigen::Matrix4f m, m_backUp;
	    m_backUp(0) = 0.998868;    m_backUp(1) = -0.0473491;    m_backUp(2) = 0.00443143;
    	    m_backUp(4) = 0.0474501;    m_backUp(5) = 0.998522;    m_backUp(6) = -0.02649;
    	    m_backUp(8) = -0.00317053;    m_backUp(9) = 0.0266703;    m_backUp(10) = 0.999639;
    	    m_backUp(12) = 0.0531114;    m_backUp(13) = -50.5847/1000;    m_backUp(14) = 34.7588/1000;
    	    m_backUp(3) = 0;    m_backUp(7) = 0;    m_backUp(11) = 0;  m_backUp(15) = 1; 
            std::cout << "m_backUp: \n" << m_backUp << std::endl;
	    pcl::PointCloud<PointA>::Ptr object(new pcl::PointCloud<PointA>());
    	    pcl::io::loadPCDFile(object_path, *object);

	    bool detected = detectConveyourBeltAndRotorcaps(resp, false, m);

	    //check if it is not a wrong conveyor belt
	    if (abs(m(12) - m_backUp(12)) > 0.1){ m = m_backUp;
pcl::console::print_error("USING!");}
            
            pcl::PointCloud<PointT> outSmall;
	    if (detected) outSmall = cutConveyourBelt(object, stereo_pointCloud, m);
	    else {
		pcl::console::print_error("Using predefined m, stereo point cloud %d and carmine %d\n", stereo_pointCloud.size(), carmine_pointCloud.size());
		outSmall = cutConveyourBelt(object, stereo_pointCloud, m);
	    }

	   
	   bool rotorcaps_detected = detectRotorcaps(outSmall, resp, constr_conveyor, false);
	   if (!rotorcaps_detected) {
		for(int r = 0; r < 10; r++) {		
			pcl::console::print_error("Finding rotorcaps again\n");
			outSmall = cutConveyourBelt(object, stereo_pointCloud, m);
			rotorcaps_detected = detectRotorcaps(outSmall, resp, constr_conveyor, false);
		if (rotorcaps_detected) break;
		}
	   }
        }
        else pcl::console::print_error("Cannot grasp frame from carmine & stereo! Are you sure they are running?! stereo size: %d carmine size: %d\n", stereo_pointCloud.size(), carmine_pointCloud.size());
//-------------------------------------------------------

    }
    else if (scenario == "detect_only_rotorcaps"){
        ros::NodeHandle nh("~");
        std::string pcddir;
        nh.getParam("conveyor_belt_2PCD", object_path);
        nh.getParam("pcddir", pcddir);

            pcl::PointCloud<PointA>::Ptr object(new pcl::PointCloud<PointA>());
            pcl::io::loadPCDFile(object_path, *object);

            if (stereo_pointCloud.size() > 1 && carmine_pointCloud.size()> 0){
                pcl::io::savePCDFile(pcddir+"carmine_PC.pcd", carmine_pointCloud);
                pcl::io::savePCDFileBinary(pcddir+"stereo_PC_binary.pcd", stereo_pointCloud);
                pcl::io::savePCDFile(pcddir+"stereo_PC.pcd", stereo_pointCloud);

                pcl::PointCloud<pcl::PointXYZRGBA> outSmall = cutConveyourBelt(object, stereo_pointCloud, keep_latest_best_pose);
                detectRotorcaps(outSmall, resp, constr_conveyor, false);
            }
        } else {
            pcl::console::print_error("There is no conveyour pose! Get conveyour pose first!\n");
        }

    return true;
}
//------------------------------------------------------------------------------------------------------------------------
void fillConstrains(){
    constr_conveyor.push_back(-0.01395);
    constr_conveyor.push_back(-0.76624);
    constr_conveyor.push_back(-0.64241);

    constr_conveyor.push_back(-0.043472);
    constr_conveyor.push_back(-0.018472);
    constr_conveyor.push_back(0.856371);
    constr_conveyor.push_back(0.05);
    //-----------------------------------
    constr_table.push_back(-0.02571);
    constr_table.push_back(-0.7486);
    constr_table.push_back(-0.6624);

    constr_table.push_back(-0.301625);
    constr_table.push_back(-0.207056);
    constr_table.push_back(1.191860);
    constr_table.push_back(0.15);

}


/*
 * Main entry point
 */
int main(int argc, char **argv) {

    // setup node
    // Initialize node
    const std::string name = "inSceneDetector";
    ros::init(argc, argv, name);
    ros::NodeHandle nh("~");
    ROS_INFO("waiting for service/getPose");
    getPose = nh.serviceClient<MsgT>("/service/getPose");
    stereoPointCloudSaver(nh, "/pikeBack");
    kinectPointCloudSaver(nh, "/carmine1");
    ROS_INFO("Starting services!");


    // Start
    fillConstrains();
   
    ros::ServiceServer servglobal = nh.advertiseService<PoseEstimation::Request, PoseEstimation::Response>("detect", pose_estimation_service);

    publish_for_vizualizer = nh.advertise<PoseEstimation::Response>("vizualize", 1000);
    ros::spin();

    return 0;
}
