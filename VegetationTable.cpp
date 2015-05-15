/***********************************************************************
VegetationTable - Class to simulate vegetation.
***********************************************************************/

#include "VegetationTable.h"

#include <stdarg.h>
#include <string>
#include <iostream>
#include <Misc/ThrowStdErr.h>
#include <Math/Math.h>
#include <Geometry/AffineCombiner.h>
#include <Geometry/Vector.h>
#include <GL/gl.h>
#include <GL/Extensions/GLARBDrawBuffers.h>
#include <GL/Extensions/GLARBFragmentShader.h>
#include <GL/Extensions/GLARBMultitexture.h>
#include <GL/Extensions/GLARBShaderObjects.h>
#include <GL/Extensions/GLARBTextureFloat.h>
#include <GL/Extensions/GLARBTextureRectangle.h>
#include <GL/Extensions/GLARBTextureRg.h>
#include <GL/Extensions/GLARBVertexShader.h>
#include <GL/Extensions/GLEXTFramebufferObject.h>
#include <GL/GLContextData.h>
#include <GL/GLTransformationWrappers.h>

#include "SurfaceRenderer.h"

namespace {

/****************
Helper functions:
****************/

GLfloat* makeBuffer(int width,int height,int numComponents,...)
	{
	va_list ap;
	va_start(ap,numComponents);
	GLfloat fill[4];
	for(int i=0;i<numComponents&&i<4;++i)
		fill[i]=GLfloat(va_arg(ap,double));
	va_end(ap);
	
	GLfloat* buffer=new GLfloat[height*width*numComponents];
	GLfloat* bPtr=buffer;
	for(int y=0;y<height;++y)
		for(int x=0;x<width;++x,bPtr+=numComponents)
			for(int i=0;i<numComponents;++i)
				bPtr[i]=fill[i];
	
	return buffer;
	}

GLhandleARB compileVertexShader(const char* shaderFileName)
	{
	/* Construct the full shader source file name: */
	std::string fullShaderFileName=SHADERDIR;
	fullShaderFileName.push_back('/');
	fullShaderFileName.append(shaderFileName);
	fullShaderFileName.append(".vs");
	
	/* Compile and return the vertex shader: */
	return glCompileVertexShaderFromFile(fullShaderFileName.c_str());
	}

GLhandleARB compileFragmentShader(const char* shaderFileName)
	{
	/* Construct the full shader source file name: */
	std::string fullShaderFileName=SHADERDIR;
	fullShaderFileName.push_back('/');
	fullShaderFileName.append(shaderFileName);
	fullShaderFileName.append(".fs");
	
	/* Compile and return the fragment shader: */
	return glCompileFragmentShaderFromFile(fullShaderFileName.c_str());
	}
}

/**************************************
Methods of class VegetationTable2::DataItem:
**************************************/
VegetationTable::DataItem::DataItem(void)
	:vegetationTextureObject(0), vegetationFramebufferObject(0), vegetationShader(0)
	{
	/* Check for and initialize all required OpenGL extensions: */
	bool supported=GLARBDrawBuffers::isSupported();
	supported=supported&&GLARBFragmentShader::isSupported();
	supported=supported&&GLARBMultitexture::isSupported();
	supported=supported&&GLARBShaderObjects::isSupported();
	supported=supported&&GLARBTextureFloat::isSupported();
	supported=supported&&GLARBTextureRectangle::isSupported();
	supported=supported&&GLARBTextureRg::isSupported();
	supported=supported&&GLARBVertexShader::isSupported();
	supported=supported&&GLEXTFramebufferObject::isSupported();
	if(!supported)
		Misc::throwStdErr("VegetationTable2: Required functionality not supported by local OpenGL");
	GLARBDrawBuffers::initExtension();
	GLARBFragmentShader::initExtension();
	GLARBMultitexture::initExtension();
	GLARBShaderObjects::initExtension();
	GLARBTextureFloat::initExtension();
	GLARBTextureRectangle::initExtension();
	GLARBTextureRg::initExtension();
	GLARBVertexShader::initExtension();
	GLEXTFramebufferObject::initExtension();
	}
	}

VegetationTable::DataItem::~DataItem(void)
	{
	// glDeleteObjectARB(...)
	glDeleteTextures(1, &vegetationTextureObject);
	glDeleteFramebuffersEXT(1, &vegetationFramebufferObject);
	glDeleteObjectARB(vegetationShader);
	}

/************************************
Static elements of class VegetationTable2:
************************************/

const char* VegetationTable2::vertexShaderSource="\
	void main()\n\
		{\n\
		/* Use standard vertex position: */\n\
		gl_Position=ftransform();\n\
		}\n";

/****************************
Methods of class VegetationTable2:
****************************/

// calcDerivative

VegetationTable::VegetationTable(
		GLsizei width, 
		GLsizei height, 
		const Plane& basePlane,
		const Point basePlaneCorners[4])
	{
	/* Initialize the vegetation table size: */
	size[0]=width;
	size[1]=height;

	/* Project the corner points to the base plane and calculate their centroid: */
	Point bpc[4];
	Point::AffineCombiner centroidC;
	for(int i=0;i<4;++i)
		{
		bpc[i]=basePlane.project(basePlaneCorners[i]);
		centroidC.addPoint(bpc[i]);
		}
	Point baseCentroid=centroidC.getPoint();
	
	/* Calculate the transformation from camera space to upright elevation model space: */
	typedef Point::Vector Vector;
	Vector z=basePlane.getNormal();
	Vector x=(bpc[1]-bpc[0])+(bpc[3]-bpc[2]);
	Vector y=Geometry::cross(z,x);
	baseTransform=Transform::translateFromOriginTo(baseCentroid);
	baseTransform*=Transform::rotate(Transform::Rotation::fromBaseVectors(x,y));
	baseTransform.doInvert();
	
	/* Calculate the domain of upright elevation model space: */
	domain=Box::empty;
	for(int i=0;i<4;++i)
		domain.addPoint(baseTransform.transform(bpc[i]));
	domain.min[2]=Scalar(-20);
	domain.max[2]=Scalar(100);
	
	/* Calculate the grid's cell size: */
	for(int i=0;i<2;++i)
		cellSize[i]=GLfloat((domain.max[i]-domain.min[i])/Scalar(size[i]));
	std::cout<<cellSize[0]<<" x "<<cellSize[1]<<std::endl;

	/* Initialize simulations parameters */
	// ...
	}

VegetationTable::~VegetationTable(void)
	{
	}

void VegetationTable::initContext(GLContextData& contextData) const
	{
	DataItem* dataItem=new DataItem;
	contextData.addDataItem(this,dataItem);
	
	glActiveTextureARB(GL_TEXTURE0_ARB);

	// Generate framebuffers here, both for render and computations
	
	{
	/* Create the cell-centered vegetation texture: */
	glGenTextures(1,&dataItem->vegetationTextureObject);
	glBindTexture(GL_TEXTURE_RECTANGLE_ARB,dataItem->vegetationTextureObject);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_WRAP_S,GL_CLAMP);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_WRAP_T,GL_CLAMP);
	GLfloat* w=makeBuffer(size[0],size[1],1,0.0);
	glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_R32F,size[0],size[1],0,GL_LUMINANCE,GL_FLOAT,w);
	delete[] w;
	}

	/* Protect the newly-created textures: */
	glBindTexture(GL_TEXTURE_RECTANGLE_ARB,0);

	/* Save the currently bound frame buffer: */
	GLint currentFrameBuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT,&currentFrameBuffer);

	/* Create the vegetation frame buffer: */
	glGenFramebuffersEXT(1,&dataItem->vegetationFramebufferObject);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,dataItem->vegetationFramebufferObject);
	
	/* Attach the vegetation texture to the vegetation frame buffer: */
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,dataItem->vegetationTextureObject,0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
	glReadBuffer(GL_NONE);
	}
	
	/* Restore the previously bound frame buffer: */
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,currentFrameBuffer);
	}

	/* Create the vegetation shader: */
	{
	GLhandleARB vertexShader=glCompileVertexShaderFromString(vertexShaderSource);
	GLhandleARB fragmentShader=compileFragmentShader("VegetationUpdateShader");
	dataItem->vegetationShader=glLinkShader(vertexShader,fragmentShader);
	glDeleteObjectARB(vertexShader);
	glDeleteObjectARB(fragmentShader);
	dataItem->vegetationShaderUniformLocations[0]=glGetUniformLocationARB(dataItem->vegetationShader,"vegetationSampler");

	}
}

void VegetationTable2::setElevationRange(VegetationTable2::Scalar newMin,VegetationTable2::Scalar newMax)
	{
	domain.min[2]=newMin;
	domain.max[2]=newMax;
	}

// Method for running a simulation step