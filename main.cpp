#include <ma.h>
#include <mavsprintf.h>
#include <MAUtil/Moblet.h>
#include <NativeUI/Widgets.h>
#include <NativeUI/WidgetUtil.h>
#include <GLES/gl.h>
#include "MAHeaders.h"
#include <madmath.h>

using namespace MAUtil;
using namespace NativeUI;

#define NUM_SEGMENTS 100
#define TEXTURE_REPEATS 10
#define ENGINE_ACC 500
#define GRAVITY_ACC 100
#define MAX_SPEED 3000
#define LANDING_SPEED 500
#define LABEL_UPDATE_PER 300
#define SCALE 1000

struct landSegment{
	GLfloat vcoords[4][3];
	GLfloat tcoords[4][2];
};

struct vector{
	int x;
	int y;
	int z;
};

struct vectorf{
	float x;
	float y;
	float z;
};

// A simple low pass filter used to
// smoothen the noisy accelerometer
// data.
struct LowPassFilter {
	LowPassFilter() :
		// this constant sets the cutoff for the filter.
		// It must be a value between 0 and 1, where
		// 0 means no filtering (everything is passed through)
		// and 1 that no signal is passed through.
		a(0.80f)
	{
		b = 1.0f - a;
	}

	vectorf filter(const vectorf& in) {
		previousState.x = (in.x * b) + (previousState.x * a);
		previousState.y = (in.y * b) + (previousState.y * a);
		previousState.z = (in.z * b) + (previousState.z * a);
		return previousState;
	}

	float a, b;
	vectorf previousState;
};
/**
 * Moblet to be used as a template for a Native UI application.
 */
class NativeUIMoblet : public Moblet, public GLViewListener,public SensorListener, public TimerListener
{
public:
	/**
	 * The constructor creates the user interface.
	 */
	NativeUIMoblet()
	{
		mPrevTime = maGetMilliSecondCount();
		createUI();
		createLandscape();

		mPosition.x = 0;
		mPosition.y = 0;
		mPosition.z = 90000;

		mVelocity.x=0;
		mVelocity.y=0;
		mVelocity.z=0;

		mGravity.x=0;
		mGravity.y=0;
		mGravity.z = -GRAVITY_ACC;

		mAcceleration.x = 0;
		mAcceleration.y = 0;
		mAcceleration.z = 0;

		mEnginesRunning = false;

		mFacing.x=0;
		mFacing.y=0;
		mFacing.z = -1;
		Environment::getEnvironment().addTimer(this,10,0);
		Environment::getEnvironment().addSensorListener(this);
		maSensorStart(1, -1);
	}

	/**
	 * Destructor.
	 */
	virtual ~NativeUIMoblet()
	{
		// All the children will be deleted.
		delete mScreen;
	}

	/**
	 * Create the user interface.
	 */
	void createUI()
	{
		MAExtent ex = maGetScrSize();
		int screenWidth = EXTENT_X(ex);
		int screenHeight = EXTENT_Y(ex);
		// Create a NativeUI screen that will hold layout and widgets.
		mScreen = new Screen();

		mLabel = new Label();
		mLabel->fillSpaceHorizontally();
		mLabel->fillSpaceVertically();
		mLabel->setMaxNumberOfLines(4);
		mLabel->setFontSize(12);
		//The widget that renders the animation
		mGLView = new GLView(MAW_GL_VIEW);
		mGLView->addGLViewListener(this);
		mGLView->fillSpaceHorizontally();
		mGLView->setHeight(screenHeight * 0.85);

		VerticalLayout *vLayout = new VerticalLayout();
		vLayout->fillSpaceHorizontally();
		vLayout->fillSpaceVertically();
		vLayout->addChild(mLabel);
		vLayout->addChild(mGLView);

		//Add the layout to the screen
		mScreen->setMainWidget(vLayout);

		//Show the screen
		mScreen->show();
	}

	void createLandscape()
	{
		GLfloat baseVCoords[4][3];
		GLfloat baseTCoords[4][2];

		baseTCoords[0][0] = 0.0f;  baseTCoords[0][1] = 0.0f;
		baseVCoords[0][0] = -1.0f; baseVCoords[0][1] = -1.0f; baseVCoords[0][2] = 0.0f;
		baseTCoords[1][0] = 1.0f;  baseTCoords[1][1] = 0.0f;
		baseVCoords[1][0] = 1.0f;  baseVCoords[1][1] = -1.0f; baseVCoords[1][2] = 0.0f;
		baseTCoords[2][0] = 1.0f;  baseTCoords[2][1] = 1.0f;
		baseVCoords[2][0] = 1.0f;  baseVCoords[2][1] = 1.0f; baseVCoords[2][2] = 0.0f;
		baseTCoords[3][0] = 0.0f;  baseTCoords[3][1] = 1.0f;
		baseVCoords[3][0] = -1.0f; baseVCoords[3][1] = 1.0f; baseVCoords[3][2] = 0.0f;

		float segmentsPerTexture = (float)NUM_SEGMENTS / TEXTURE_REPEATS;
		for(int x = 0; x < NUM_SEGMENTS; x++)
		{
			for(int y = 0; y < NUM_SEGMENTS; y++)
			{
				for(int i = 0; i < 4; i++)
				{
					mLandscape[x][y].vcoords[i][0] = 200000.0f * ((x - NUM_SEGMENTS/2) * 2 + baseVCoords[i][0]) / NUM_SEGMENTS;
					mLandscape[x][y].vcoords[i][1] = 200000.0f * ((y - NUM_SEGMENTS/2) * 2 + baseVCoords[i][1]) / NUM_SEGMENTS;
					mLandscape[x][y].vcoords[i][2] = 10000.0f * getPointHeight(mLandscape[x][y].vcoords[i][0], mLandscape[x][y].vcoords[i][1]);

					mLandscape[x][y].tcoords[i][0] = ((x % TEXTURE_REPEATS) / segmentsPerTexture) + ((1.0f / segmentsPerTexture) * baseTCoords[i][0]); //* 0.8f;
					mLandscape[x][y].tcoords[i][1] = ((y % TEXTURE_REPEATS) / segmentsPerTexture) + ((1.0f / segmentsPerTexture) * baseTCoords[i][1]); //* 0.8f;
				}
			}
		}
	}

	float getPointHeight(float x, float y)
	{
		float freq = 0.00003;
		float value = (cos(y*freq*2*M_PI) + cos(x*freq*2*M_PI))/2;
		return (value>0)?value:0;
	}

	void glViewReady(GLView* glView)
	{
		//Set this GLView to receive OpenGL commands
		mGLView->bind();

		// Create the texture we will use for rendering.
		createTexture();

		// Initialize OpenGL.
		initGL();
	}

	void createTexture()
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
	void initGL()
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
	void setViewport(int width, int height)
	{
		// Set the viewport to fill the GLView
		glViewport(0, 0, (GLint)width, (GLint)height);

		// Select the projection matrix.
		glMatrixMode(GL_PROJECTION);

		// Reset the projection matrix.
		glLoadIdentity();

		GLfloat ratio = (GLfloat)width / (GLfloat)height;
		gluPerspective(45.0f, ratio, 100.0f, 100000.0f);
	}

	/**
	 * Standard OpenGL utility function for setting up the
	 * perspective projection matrix.
	 */
	void gluPerspective(
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

	/**
	 * Called when a key is pressed.
	 */
	void keyPressEvent(int keyCode, int nativeCode)
	{
		if (MAK_BACK == keyCode || MAK_0 == keyCode)
		{
			// Call close to exit the application.
			close();
		}
	}

	void sensorEvent(MASensor a)
	{

		mFacing.x = -a.values[0];
		mFacing.y = a.values[1];
		mFacing.z = a.values[2];

		mFacing = mFilter.filter(mFacing);
	}

	void runTimerEvent()
	{
		//Execute only if the screen is active
		if(mEnvironmentInitialized)
		{
			//Get the current system time
			int currentTime = maGetMilliSecondCount();
			int period = currentTime-mPrevTime;
			calculateAcceleration(period/1000.0f);
			calculatePosition(period/1000.0f);
			//Calculate and draw the positions for the new frame
			checkCollision();
			draw(currentTime);
			mSecondsSinceLastUpdate += period;
			if(mSecondsSinceLastUpdate > LABEL_UPDATE_PER)
			{
				mSecondsSinceLastUpdate = 0;
				char buffer[256];
				int absSpeed = sqrt(mVelocity.x*mVelocity.x + mVelocity.y*mVelocity.y + mVelocity.z*mVelocity.z);
				sprintf(buffer,
				" Position - x:%d, y:%d, z:%d\n Speed - x:%d, y:%d, z:%d\n Absolute speed:%d, altitude:%d\n Segment - x:%d, y:%d",
						mPosition.x,mPosition.y,mPosition.z,mVelocity.x,mVelocity.y,mVelocity.z,absSpeed,mAltitude,mX,mY);
				mLabel->setText(buffer);
			}
			mPrevTime = currentTime;
		}
	}

	void calculateAcceleration(float period)
	{
		float enginePower;
		if(mEnginesRunning)
		{
			enginePower = ENGINE_ACC;
		}
		else
		{
			enginePower = 0;
		}
		mAcceleration.x = -mFacing.x * enginePower;
		mAcceleration.y = mFacing.y * enginePower;
		mAcceleration.z = -mFacing.z * enginePower;
	}

	void calculatePosition(float period)
	{
		mVelocity.x += (mGravity.x + mAcceleration.x) * period;
		mVelocity.y += (mGravity.y + mAcceleration.y) * period;
		mVelocity.z += (mGravity.z + mAcceleration.z) * period;

		float velocity = sqrt(mVelocity.x*mVelocity.x + mVelocity.y*mVelocity.y + mVelocity.z*mVelocity.z);

		if(velocity > MAX_SPEED)
		{
			mVelocity.x = MAX_SPEED * (float)mVelocity.x/velocity;
			mVelocity.y = MAX_SPEED * (float)mVelocity.y/velocity;
			mVelocity.z = MAX_SPEED * (float)mVelocity.z/velocity;
		}

		mPosition.x += mVelocity.x * period;
		mPosition.y += mVelocity.y * period;
		mPosition.z += mVelocity.z * period;
	}

	void checkCollision()
	{
		/*for(int x = 0; x < NUM_SEGMENTS; x++)
		{
			for(int y = 0; y < NUM_SEGMENTS; y++)
			{
				landSegment* segment = &mLandscape[x][y];
				if(	segment->vcoords[0][0] < mPosition.x &&
					segment->vcoords[2][0] > mPosition.x &&
					segment->vcoords[0][1] < mPosition.y &&
					segment->vcoords[2][1] > mPosition.y)
				{
					mX = x;
					mY = y;
					vector plane[3];
					float dx1 = segment->vcoords[1][0] - mPosition.x;
					float dy1 = segment->vcoords[1][1] - mPosition.y;
					float dx3 = segment->vcoords[3][0] - mPosition.x;
					float dx3 = segment->vcoords[3][1] - mPosition.y;
					float d1 = dx1 * dx1 + dy1 * dy1;
					float d3 = dx3 * dx3 + dy3 * dy3;
					if(d1 < d3)
					{
						plane[0].x = segment->vcoords[0][0];
						plane[0].y = segment->vcoords[0][1];
						plane[0].z = segment->vcoords[0][2];

						plane[1].x = segment->vcoords[1][0];
						plane[1].y = segment->vcoords[1][1];
						plane[1].z = segment->vcoords[1][2];

						plane[2].x = segment->vcoords[2][0];
						plane[2].y = segment->vcoords[2][1];
						plane[2].z = segment->vcoords[2][2];
					}
					else
					{
						plane[0].x = segment->vcoords[0][0];
						plane[0].y = segment->vcoords[0][1];
						plane[0].z = segment->vcoords[0][2];

						plane[1].x = segment->vcoords[2][0];
						plane[1].y = segment->vcoords[2][1];
						plane[1].z = segment->vcoords[2][2];

						plane[2].x = segment->vcoords[3][0];
						plane[2].y = segment->vcoords[3][1];
						plane[2].z = segment->vcoords[3][2];
					}
					vector planeVector;
					planeVector.x = (plane[0].x + plane[1].x + plane[2].x) / 3;
					planeVector.y = (plane[0].y + plane[1].y + plane[2].y) / 3;
					planeVector.z = (plane[0].z + plane[1].z + plane[2].z) / 3;


				}
			}
		}*/
	}

	void draw(int currentTime)
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

	void renderLandscape()
	{
		// Array used to convert from QUAD to TRIANGLE_STRIP.
		// QUAD is not available on the OpenGL implementation
		// we are using.
		GLubyte indices[4] = {0, 1, 3, 2};

		// Select the texture to use when rendering the box.
		glBindTexture(GL_TEXTURE_2D, mLunarTexture);


		glPushMatrix();

		glRotatef(180.0f * asin(mFacing.y)/M_PI, 1.0f, 0.0f, 0.0f);
		glRotatef(180.0f * asin(mFacing.x)/M_PI, 0.0f, 1.0f, 0.0f);

		glTranslatef(-mPosition.x, -mPosition.y, -mPosition.z);
		//glScalef(20.0f, 20.0f, 0.0f);



		// Enable texture and vertex arrays
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		//glEnableClientState( GL_COLOR_ARRAY );
		// Set up the color array.
		/*GLfloat colors[4*4] = {
				1.0f, 0.0f, 0.0f, 1.0f,
				0.0f, 1.0f, 0.0f, 1.0f,
				0.0f, 0.0f, 1.0f, 1.0f,
				1.0f, 1.0f, 0.0f, 1.0f
			};*/
		for(int x = 0; x < NUM_SEGMENTS; x++) {
			for(int y = 0; y < NUM_SEGMENTS; y++) {
				// Set pointers to vertex coordinates and texture coordinates.
				glVertexPointer(3, GL_FLOAT, 0, mLandscape[x][y].vcoords);
				glTexCoordPointer(2, GL_FLOAT, 0, mLandscape[x][y].tcoords);
				//glColorPointer(4, GL_FLOAT, 0, colors);


				// This draws the segmen.
				glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, indices);


			}
		}
		glPopMatrix();
		// Disable texture and vertex arrays
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);


	}

	virtual void pointerPressEvent(MAPoint2d p)
	{
		mEnginesRunning = true;
	}

	virtual void pointerReleaseEvent(MAPoint2d p)
	{
		mEnginesRunning = false;
	}

private:
    Screen* mScreen;			//A Native UI screen
    Label* mLabel;
    GLView* mGLView;
    int mX, mY;
    int mAltitude;
    int mSecondsSinceLastUpdate;
    int mPrevTime;
    GLuint mLunarTexture;
    bool mEnvironmentInitialized;
    landSegment mLandscape[NUM_SEGMENTS][NUM_SEGMENTS];
    vector mVelocity;
    vector mPosition;
    vector mGravity;
    vectorf mFacing;
    vector mAcceleration;
    bool mEnginesRunning;
    LowPassFilter mFilter;
};

/**
 * Main function that is called when the program starts.
 */
extern "C" int MAMain()
{
	Moblet::run(new NativeUIMoblet());
	return 0;
}
