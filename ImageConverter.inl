/******************************************************************************
*       SOFA, Simulation Open-Framework Architecture, version 1.0 RC 1        *
*                (c) 2006-2011 MGH, INRIA, USTL, UJF, CNRS                    *
*                                                                             *
* This library is free software; you can redistribute it and/or modify it     *
* under the terms of the GNU Lesser General Public License as published by    *
* the Free Software Foundation; either version 2.1 of the License, or (at     *
* your option) any later version.                                             *
*                                                                             *
* This library is distributed in the hope that it will be useful, but WITHOUT *
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or       *
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License *
* for more details.                                                           *
*                                                                             *
* You should have received a copy of the GNU Lesser General Public License    *
* along with this library; if not, write to the Free Software Foundation,     *
* Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.          *
*******************************************************************************
*                               SOFA :: Modules                               *
*                                                                             *
* Authors: The SOFA Team and external contributors (see Authors.txt)          *
*                                                                             *
* Contact information: contact@sofa-framework.org                             *
******************************************************************************/

#define SOFA_RGBDTRACKING_IMAGECONVERTER_INL

#include <sofa/core/visual/VisualParams.h>
#include <sofa/core/objectmodel/BaseContext.h>
#include <sofa/core/Mapping.inl>
#include <sofa/simulation/Simulation.h>
#include <sofa/core/topology/BaseMeshTopology.h>
#include <sofa/gui/BaseGUI.h>
#include <sofa/gui/BaseViewer.h>
#include <sofa/gui/GUIManager.h>

#ifdef USING_OMP_PRAGMAS
#include <omp.h>
#endif

#include <limits>
#include <iterator>
#include <sofa/helper/gl/Color.h>

#ifdef Success
  #undef Success
#endif

#include <pcl/common/common_headers.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/registration/icp.h>
#include <pcl/common/transforms.h>

#include "ImageConverter.h"

using std::cerr;
using std::endl;

namespace sofa
{

namespace core
{

namespace objectmodel
{

using namespace sofa::defaulttype;
using namespace helper;

template <class DataTypes, class DepthTypes>
ImageConverter<DataTypes, DepthTypes>::ImageConverter()
    : Inherit()
    , depthImage(initData(&depthImage,DepthTypes(),"depthImage","depth map"))
    //, depthTransform(initData(&depthTransform, TransformType(), "depthTransform" , ""))
    , image(initData(&image,ImageTypes(),"image","image"))
    //, transform(initData(&transform, TransformType(), "transform" , ""))
    , useRealData(initData(&useRealData,true,"useRealData","Use real data"))
    , useSensor(initData(&useSensor,false,"useSensor","Use the sensor"))
    , sensorType(initData(&sensorType, 0,"sensorType","Type of the sensor"))
    , niterations(initData(&niterations,3,"niterations","Number of iterations in the tracking process"))
{
    //softk.init();
    this->f_listening.setValue(true);
    this->addAlias(&depthImage, "depthImage");
    depthImage.setGroup("depthImage");
    depthImage.setReadOnly(true);
    this->addAlias(&image, "image");
    image.setGroup("image");
    image.setReadOnly(true);
    //niterations.setValue(2);
}

template <class DataTypes, class DepthTypes>
ImageConverter<DataTypes, DepthTypes>::~ImageConverter()
{
}

template <class DataTypes, class DepthTypes>
void ImageConverter<DataTypes, DepthTypes>::init()
{
    this->Inherit::init();
    core::objectmodel::BaseContext* context = this->getContext();
    mstate = dynamic_cast<sofa::core::behavior::MechanicalState<DataTypes> *>(context->getMechanicalState());

    getImages();
		
}

template<class DataTypes, class DepthTypes>
void ImageConverter<DataTypes, DepthTypes>::getImages()
{    
    //int t = (int)this->getContext()->getTime();
    //cv::Rect ROI(160, 120, 320, 240);

    //if (t%niterations.getValue() == 0)
    {
	raDepth rdepth(this->depthImage);
	if( rdepth->isEmpty() || !mstate)  return;

	const CImg<dT>& depthimg =rdepth->getCImg(0); 
        cv::Mat depth0,depthuc;
	int width, height;		

        height = depthimg.height();
        width = depthimg.width();

        //std::cout << " height0 " << height << std::endl;
        //std::cout << " width0 " << width << std::endl;
        double timeAcq0 = (double)getTickCount();
        //cv::Mat depth_single = cv::Mat::zeros(height,width,CV_32FC1);
        //memcpy(depth_single.data, (float*)depthimg.data(), height*width*sizeof(float));
        depth = cv::Mat::zeros(height,width,CV_32FC1);
        memcpy(depth.data, (float*)depthimg.data(), height*width*sizeof(float));
        double timeAcq1 = (double)getTickCount();
        //cout <<"time imconv 0 " << (timeAcq1 - timeAcq0)/getTickFrequency() << endl;

        /*switch (sensorType.getValue())
        {
        // FLOAT ONE CHANNEL
            case 0:
            cv::resize(depth_single, depth, cv::Size(width/2, height/2), 0, 0);
            break;
            case 1:
            depth = depth_single(ROI);
            break;
        }*/

	raImage rimg(this->image);

        if( rimg->isEmpty() || !mstate)  return;

        const CImg<T>& img =rimg->getCImg(0);
        //cv::Mat color0;
        //color0 = cv::Mat::zeros(img.height(),img.width(), CV_8UC3);

	 color_1 = color;
        //color_2 = color;

        color = cv::Mat::zeros(img.height(),img.width(), CV_8UC3);
        timeAcq0 = (double)getTickCount();

        if(img.spectrum()==3)
        {
            unsigned char* rgb = (unsigned char*)color.data;
            const unsigned char *ptr_r = img.data(0,0,0,2), *ptr_g = img.data(0,0,0,1), *ptr_b = img.data(0,0,0,0);
            for ( int siz = 0 ; siz<img.width()*img.height(); siz++)    {*(rgb++) = *(ptr_r++) ; *(rgb++) = *(ptr_g++); *(rgb++) = *(ptr_b++); }
        }
	
	
        /*switch (sensorType.getValue())
        {
            case 0:
            cv::resize(color0, color, depth.size(), 0, 0);
            break;
            case 1:
            color = color0(ROI);
            break;
        }*/

        timeAcq1 = (double)getTickCount();

        //cout <<"time imconv 1 " << (timeAcq1 - timeAcq0)/getTickFrequency() << endl;

        //cv::imwrite("color01.png", color);
    }
}

template <class DataTypes, class DepthTypes>
void ImageConverter<DataTypes, DepthTypes>::handleEvent(sofa::core::objectmodel::Event *event)
{
    int t = (int)this->getContext()->getTime();
        if (dynamic_cast<simulation::AnimateBeginEvent*>(event))
	{
    		if (useRealData.getValue() && useSensor.getValue() )
    		getImages();
	}
		
}


template<class DataTypes, class DepthTypes>
void ImageConverter<DataTypes, DepthTypes>::draw(const core::visual::VisualParams* vparams)
{	


}

}
}
} // namespace sofa



