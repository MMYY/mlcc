#include <iostream>
#include <fstream>
#include <string>

#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <sensor_msgs/PointCloud2.h>
#include <Eigen/StdVector>
#include <Eigen/Dense>
#include "ros/ros.h"

#include "mypcl.hpp"
#include "extrinsic_refine.hpp"

using namespace std;
using namespace Eigen;
double voxel_size;

void cut_voxel(unordered_map<VOXEL_LOC, OCTO_TREE*>& feature_map,
			   pcl::PointCloud<PointType>::Ptr feature_pts,
			   Eigen::Quaterniond q, Eigen::Vector3d t, int f_head, int capacity,
               bool is_base_lidar = true)
{
	uint pt_size = feature_pts->size();
	for (uint i = 0; i < pt_size; i++)
	{
		PointType& pt = feature_pts->points[i];
		Eigen::Vector3d pt_origin(pt.x, pt.y, pt.z);
		Eigen::Vector3d pt_trans = q * pt_origin + t;

		float loc_xyz[3];
		for (int j = 0; j < 3; j++)
		{
			loc_xyz[j] = pt_trans[j] / voxel_size;
			if (loc_xyz[j] < 0)
				loc_xyz[j] -= 1.0;
		}

		VOXEL_LOC position((int64_t)loc_xyz[0], (int64_t)loc_xyz[1], (int64_t)loc_xyz[2]);
		auto iter = feature_map.find(position);
		if (iter != feature_map.end())
		{
            if (is_base_lidar)
            {
                iter->second->baseOriginPc[f_head]->emplace_back(pt_origin);
			    iter->second->baseTransPc[f_head]->emplace_back(pt_trans);
            }
			else
            {
                iter->second->refOriginPc[f_head]->emplace_back(pt_origin);
			    iter->second->refTransPc[f_head]->emplace_back(pt_trans);
            }
			iter->second->is2opt = true;
		}
		else
		{
			OCTO_TREE *ot = new OCTO_TREE(capacity);
            if (is_base_lidar)
            {
                ot->baseOriginPc[f_head]->emplace_back(pt_origin);
			    ot->baseTransPc[f_head]->emplace_back(pt_trans);
            }
            else
            {
                ot->refOriginPc[f_head]->emplace_back(pt_origin);
			    ot->refTransPc[f_head]->emplace_back(pt_trans);
            }
			
			ot->voxel_center[0] = (0.5 + position.x) * voxel_size;
			ot->voxel_center[1] = (0.5 + position.y) * voxel_size;
			ot->voxel_center[2] = (0.5 + position.z) * voxel_size;
			ot->quater_length = voxel_size / 4.0;
			feature_map[position] = ot;
		}
	}
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "extrinsic_refine");
    ros::NodeHandle nh("~");

    ros::Publisher pub_surf = nh.advertise<sensor_msgs::PointCloud2>("/map_surf", 100);
    ros::Publisher pub_surf_debug = nh.advertise<sensor_msgs::PointCloud2>("/debug_surf", 100);

    string data_path, log_path;
    int max_iter, base_lidar, ref_lidar;
    double downsmp_sz_base, downsmp_sz_ref;

    nh.getParam("data_path", data_path);
    nh.getParam("log_path", log_path);
    nh.getParam("max_iter", max_iter);
    nh.getParam("base_lidar", base_lidar);
    nh.getParam("ref_lidar", ref_lidar);
    nh.getParam("voxel_size", voxel_size);
    nh.getParam("downsmp_sz_base", downsmp_sz_base);
    nh.getParam("downsmp_sz_ref", downsmp_sz_ref);

    sensor_msgs::PointCloud2 debugMsg, colorCloudMsg;
    vector<mypcl::pose> pose_vec = mypcl::read_pose(data_path + "pose.json");
    vector<mypcl::pose> ref_vec = mypcl::read_pose(data_path + "ref.json");
    size_t ref_size = ref_vec.size();
    size_t pose_size = pose_vec.size();
    
    ros::Time t_begin, t_end, cur_t;
    double avg_time = 0.0;

    pcl::PointCloud<PointType>::Ptr pc_debug(new pcl::PointCloud<PointType>);
    pcl::PointCloud<PointType>::Ptr pc_surf(new pcl::PointCloud<PointType>);
    pcl::PointCloud<PointType>::Ptr pc_color(new pcl::PointCloud<PointType>);

    vector<pcl::PointCloud<PointType>::Ptr> base_pc, ref_pc;
    base_pc.resize(pose_size);
    ref_pc.resize(pose_size);
    for (size_t i = 0; i < pose_size; i++)
    {
        pcl::PointCloud<PointType>::Ptr pc_base(new pcl::PointCloud<PointType>);
        pcl::PointCloud<PointType>::Ptr pc_ref(new pcl::PointCloud<PointType>);
        *pc_base = mypcl::read_pointdat(data_path + to_string(base_lidar) + 
            "/patch" + to_string(i) + ".dat");
        *pc_ref = mypcl::read_pointdat(data_path + to_string(ref_lidar) + 
            "/patch" + to_string(i) + ".dat");
        base_pc[i] = pc_base;
        ref_pc[i] = pc_ref;
    }

    for (int loop = 0; loop < max_iter; loop++)
    {
        cout << "---------------------" << endl;
        cout << "iteration " << loop << endl;
        t_begin = ros::Time::now();
        unordered_map<VOXEL_LOC, OCTO_TREE*> surf_map;
        EXTRIN_OPTIMIZER lm_opt(pose_size, ref_size);
        cur_t = ros::Time::now();

        for (size_t i = 0; i < pose_size; i++)
        {
            OCTO_TREE::voxel_windowsize = i+1;
            downsample_voxel(*base_pc[i], downsmp_sz_base);
            downsample_voxel(*ref_pc[i], downsmp_sz_ref);
            cut_voxel(surf_map, base_pc[i], pose_vec[i].q, pose_vec[i].t, i, pose_size);
            for (auto iter = surf_map.begin(); iter != surf_map.end(); ++iter)
                if (iter->second->is2opt)
                    iter->second->recut(0, i);

            cut_voxel(surf_map, ref_pc[i], pose_vec[i].q * ref_vec[0].q,
                      pose_vec[i].q * ref_vec[0].t + pose_vec[i].t, i, pose_size, false);
            for (auto iter = surf_map.begin(); iter != surf_map.end(); ++iter)
                if (iter->second->is2opt)
                    iter->second->recut(0, i);
        }

        for (size_t i = 0; i < pose_size; i++)
            assign_qt(lm_opt.poses[i], lm_opt.ts[i], pose_vec[i].q, pose_vec[i].t);

        for (size_t i = 0; i < ref_size; i++)
            assign_qt(lm_opt.refQs[i], lm_opt.refTs[i], ref_vec[i].q, ref_vec[i].t);

        for (auto iter = surf_map.begin(); iter != surf_map.end(); ++iter)
            if (iter->second->is2opt)
                iter->second->feed_pt(lm_opt);

        lm_opt.optimize();

        for (size_t i = 0; i < ref_size; i++)
            assign_qt(ref_vec[i].q, ref_vec[i].t, lm_opt.refQs[i], lm_opt.refTs[i]);

        lm_opt.free_voxel();
        for (auto iter = surf_map.begin(); iter != surf_map.end(); ++iter)
            iter->second->~OCTO_TREE();
        t_end = ros::Time::now();
        cout << "time cost " << (t_end - t_begin).toSec() << endl;
        avg_time += (t_end - t_begin).toSec();

        pc_color->clear();
        Eigen::Quaterniond q0(pose_vec[0].q.w(), pose_vec[0].q.x(),
                              pose_vec[0].q.y(), pose_vec[0].q.z());
        Eigen::Vector3d t0(pose_vec[0].t(0), pose_vec[0].t(1), pose_vec[0].t(2));
        for (size_t i = 0; i < pose_size; i++)
        {
            pc_debug->clear();
            mypcl::transform_pointcloud(*base_pc[i], *pc_debug,
                q0.inverse()*(pose_vec[i].t-t0), q0.inverse()*pose_vec[i].q);
            pc_color = mypcl::append_cloud(pc_color, *pc_debug);
            pc_debug->clear();
            mypcl::transform_pointcloud(*ref_pc[i], *pc_debug,
                q0.inverse()*(pose_vec[i].t-t0)+q0.inverse()*pose_vec[i].q*ref_vec[0].t,
                q0.inverse()*pose_vec[i].q*ref_vec[0].q);
            pc_color = mypcl::append_cloud(pc_color, *pc_debug);
        }

        pcl::toROSMsg(*pc_color, debugMsg);
        debugMsg.header.frame_id = "camera_init";
        debugMsg.header.stamp = cur_t;
        pub_surf_debug.publish(debugMsg);
    }
    
    cout << "---------------------" << endl;
    cout << "complete" << endl;
    cout << "averaged iteration time " << avg_time / max_iter << endl;
    mypcl::write_ref(ref_vec, data_path);

    pc_color->clear();
    Eigen::Quaterniond q0(pose_vec[0].q.w(), pose_vec[0].q.x(),
                          pose_vec[0].q.y(), pose_vec[0].q.z());
    Eigen::Vector3d t0(pose_vec[0].t(0), pose_vec[0].t(1), pose_vec[0].t(2));
    for (size_t i = 0; i < pose_size; i++)
    {
        pc_surf->clear();
        *pc_surf = mypcl::read_pointdat(data_path + to_string(base_lidar) + 
            "/patch" + to_string(i) + ".dat");
        mypcl::transform_pointcloud(*pc_surf, *pc_surf,
            q0.inverse()*(pose_vec[i].t-t0), q0.inverse()*pose_vec[i].q);
        pcl::toROSMsg(*pc_surf, colorCloudMsg);
        colorCloudMsg.header.frame_id = "camera_init";
        colorCloudMsg.header.stamp = cur_t;
        pub_surf.publish(colorCloudMsg);
        pc_surf->clear();
        *pc_surf = mypcl::read_pointdat(data_path + to_string(ref_lidar) + 
            "/patch" + to_string(i) + ".dat");
        mypcl::transform_pointcloud(*pc_surf, *pc_surf,
            q0.inverse()*(pose_vec[i].t-t0)+q0.inverse()*pose_vec[i].q*ref_vec[0].t,
            q0.inverse()*pose_vec[i].q*ref_vec[0].q);
        pcl::toROSMsg(*pc_surf, colorCloudMsg);
        colorCloudMsg.header.frame_id = "camera_init";
        colorCloudMsg.header.stamp = cur_t;
        pub_surf.publish(colorCloudMsg);
    }

    ros::Rate loop_rate(1);
    while (ros::ok())
    {
        ros::spinOnce();
        loop_rate.sleep();
    }
}