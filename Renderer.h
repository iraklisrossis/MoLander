/*
 * Renderer.h
 *
 *  Created on: Apr 4, 2012
 *      Author: iraklis
 */

#ifndef RENDERER_H_
#define RENDERER_H_

#include <NativeUI/GlView.h>
#include <NativeUI/GlViewListener.h>
#include <GLES/gl.h>
#include <madmath.h>

using namespace NativeUI;

struct vector{
	float x;
	float y;
	float z;
};

struct landSegment{
	GLfloat vcoords[4][3];
	GLfloat tcoords[4][2];
	vector normalVector[2];
	float distance[2];
};

struct landscape{
	int numSegments;
	landSegment *segments;
};

struct camera{
	vector position;
	vector facing;
};

class Renderer : public GLViewListener
{
public:
	void init(GLView *glView);

	void setLandscape(landscape *ls);

	void setCamera(camera *c);

	void glViewReady(GLView* glView);

	void draw();

private:
	void setViewport(int width, int height);

	void gluPerspective(
		GLfloat fovy,
		GLfloat aspect,
		GLfloat zNear,
		GLfloat zFar);

	void renderLandscape();
	// Create the texture we will use for rendering.
	void createTexture();

	// Initialize OpenGL.
	void initGL();

	GLView *mGLView;
	GLuint mLunarTexture;
	bool mEnvironmentInitialized;
	camera *mCamera;
	landscape *mLandscape;
};


#endif /* RENDERER_H_ */
