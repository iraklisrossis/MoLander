/*
 * Renderer.cpp
 *
 *  Created on: Apr 4, 2012
 *      Author: iraklis
 */

#include "Renderer.h"
#include "MAHeaders.h"


void Renderer::init(GLView *glView)
{
	mGLView = glView;
	mGLView->addGLViewListener(this);
}

void Renderer::glViewReady(GLView *glView)
{
	//Set this GLView to receive OpenGL commands
	mGLView->bind();

	// Create the texture we will use for rendering.
	createTexture();

	// Initialize OpenGL.
	initGL();
}

void Renderer::setLandscape(landscape *ls)
{
	mLandscape = ls;
}

void Renderer::setCamera(camera *c)
{
	mCamera = c;
}

void Renderer::createTexture()
{
	// Create an OpenGL 2D texture from the image resource.
	glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &mLunarTexture);
	glBindTexture(GL_TEXTURE_2D, mLunarTexture);
	maOpenGLTexImage2D(LUNAR_TEXTURE);

	// Set texture parameters.
	glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

/**
 * Standard OpenGL initialization.
 */
void Renderer::initGL()
{
	//Configure the viewport
	setViewport(mGLView->getWidth(), mGLView->getHeight());
    // Enable texture mapping.
    glEnable(GL_TEXTURE_2D);

    // Enable smooth shading.
	glShadeModel(GL_SMOOTH);

	// Set the depth value used when clearing the depth buffer.
	glClearDepthf(1.0f);

	//glEnable(GL_BLEND);

	glBlendFunc(GL_ONE, GL_ONE);

	mEnvironmentInitialized = true;
}

/**
 * Setup the projection matrix.
 */
void Renderer::setViewport(int width, int height)
{
	// Set the viewport to fill the GLView
	glViewport(0, 0, (GLint)width, (GLint)height);

	// Select the projection matrix.
	glMatrixMode(GL_PROJECTION);

	// Reset the projection matrix.
	glLoadIdentity();

	GLfloat ratio = (GLfloat)width / (GLfloat)height;
	gluPerspective(45.0f, ratio, 0.1f, 100.0f);
}

/**
 * Standard OpenGL utility function for setting up the
 * perspective projection matrix.
 */
void Renderer::gluPerspective(
	GLfloat fovy,
	GLfloat aspect,
	GLfloat zNear,
	GLfloat zFar)
{
	//const float M_PI = 3.14159;

	GLfloat ymax = zNear * tan(fovy * M_PI / 360.0);
	GLfloat ymin = -ymax;
	GLfloat xmin = ymin * aspect;
	GLfloat xmax = ymax * aspect;

	glFrustumf(xmin, xmax, ymin, ymax, zNear, zFar);
}

void Renderer::draw()
{
	if(mEnvironmentInitialized)
	{
		// Set the background color to be used when clearing the screen.
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

		// Clear the screen and the depth buffer.
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// Use the model matrix.
		glMatrixMode(GL_MODELVIEW);

		// Reset the model matrix.
		glLoadIdentity();

		renderLandscape();
		// Wait (blocks) until all GL drawing commands to finish.
		glFinish();

		mGLView->redraw();
	}
}

void Renderer::renderLandscape()
{
	// Array used to convert from QUAD to TRIANGLE_STRIP.
	// QUAD is not available on the OpenGL implementation
	// we are using.
	GLubyte indices[4] = {0, 1, 3, 2};

	// Select the texture to use when rendering the box.
	glBindTexture(GL_TEXTURE_2D, mLunarTexture);


	glPushMatrix();

	glRotatef(180.0f * asin(mCamera->facing.y)/M_PI, 1.0f, 0.0f, 0.0f);
	glRotatef(180.0f * asin(mCamera->facing.x)/M_PI, 0.0f, 1.0f, 0.0f);

	glTranslatef(-mCamera->position.x, -mCamera->position.y, -mCamera->position.z);
	//glScalef(20.0f, 20.0f, 0.0f);



	// Enable texture and vertex arrays
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	for(int i = 0; i < mLandscape->numSegments; i++) {
		// Set pointers to vertex coordinates and texture coordinates.
		glVertexPointer(3, GL_FLOAT, 0, mLandscape->segments[i].vcoords);
		glTexCoordPointer(2, GL_FLOAT, 0, mLandscape->segments[i].tcoords);

		// This draws the segment.
		glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, indices);
	}
	glPopMatrix();
	// Disable texture and vertex arrays
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
}
