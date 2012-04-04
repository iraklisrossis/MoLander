#include <ma.h>
#include <mavsprintf.h>
#include <MAFS/File.h>
#include <MAUtil/Moblet.h>
#include <NativeUI/Widgets.h>
#include <NativeUI/WidgetUtil.h>
#include "MAHeaders.h"
#include <madmath.h>
#include "LuaEngine.h"
#include "Renderer.h"
#include "BundleDownloader.h"

using namespace MAUtil;
using namespace NativeUI;

#define NUM_SEGMENTS 100
#define TEXTURE_REPEATS 10
#define ENGINE_ACC 0.5f
#define GRAVITY_ACC 0.1f
#define MAX_SPEED 3.0f
#define LANDING_SPEED 1.0f
#define LANDING_DEVIATION 0.3f
#define LABEL_UPDATE_PER 0.3f


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

	vector filter(const vector& in) {
		previousState.x = (in.x * b) + (previousState.x * a);
		previousState.y = (in.y * b) + (previousState.y * a);
		previousState.z = (in.z * b) + (previousState.z * a);
		return previousState;
	}

	float a, b;
	vector previousState;
};

/*static int TestFunc(lua_State *L)
{
	maPanic(0,"sfdsfsfsdf");

	return 1; // Number of results
}*/

/**
 * Moblet to be used as a template for a Native UI application.
 */
class NativeUIMoblet : public Moblet, public SensorListener, public TimerListener, public BundleListener
{
public:
	/**
	 * The constructor creates the user interface.
	 */
	NativeUIMoblet()
	{
		mDownloader = new BundleDownloader(this);
		initialize();
	}

	/**
	 * Destructor.
	 */
	virtual ~NativeUIMoblet()
	{
		// All the children will be deleted.
		delete mScreen;
	}

	void initialize()
	{
		mPrevTime = maGetMilliSecondCount();
		initLua();
		createUI();
		mRenderer.init(mGLView);
		mCamera = new camera;
		mRenderer.setCamera(mCamera);
		createLandscape();
		mRenderer.setLandscape(mLandscape);
		mPosition.x = 0;
		mPosition.y = 0;
		mPosition.z = 40;

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

	void initLua()
	{
		exractBin(LOCAL_FILES_BIN);
		if (!mLua.initialize())
		{
			maPanic(0,"Lua engine failed to initialize");
		}
		String initScript;
		readTextFromFile("Init.lua",initScript);
		mLua.eval(initScript.c_str());
	}

	void exractBin(MAHandle bin)
	{
		int bufferSize = 1024;
		char buffer[bufferSize];

		int size = maGetSystemProperty(
			"mosync.path.local",
			buffer,
			bufferSize);
		mLocalPath = buffer;
		setCurrentFileSystem(bin, 0);
		int result = MAFS_extractCurrentFileSystem(buffer);
		freeCurrentFileSystem();
	}

	bool readTextFromFile(
		const MAUtil::String& filePath,
		MAUtil::String& inText)
	{
		MAHandle file = maFileOpen((mLocalPath + filePath).c_str(), MA_ACCESS_READ);
		if (file < 0)
		{
			return false;
		}

		int size = maFileSize(file);
		if (size < 1)
		{
			return false;
		}

		// Allocate buffer with space for a null termination character.
		char* buffer = (char*) malloc(sizeof(char) * (size + 1));

		int result = maFileRead(file, buffer, size);

		maFileClose(file);

		buffer[size] = 0;
		inText = buffer;

		return result == 0;
	}

	virtual void bundleDownloaded(MAHandle data)
	{
		exractBin(data);
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
		mGLView->fillSpaceHorizontally();
		mGLView->setHeight((int)(screenHeight * 0.85));

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
		mLandscape = new landscape;
		mLandscape->numSegments = NUM_SEGMENTS*NUM_SEGMENTS;
		mLandscape->segments = new landSegment[mLandscape->numSegments];

		baseTCoords[0][0] = 0.0f;  baseTCoords[0][1] = 0.0f;
		baseVCoords[0][0] = -1.0f; baseVCoords[0][1] = -1.0f; baseVCoords[0][2] = 0.0f;
		baseTCoords[1][0] = 1.0f;  baseTCoords[1][1] = 0.0f;
		baseVCoords[1][0] = 1.0f;  baseVCoords[1][1] = -1.0f; baseVCoords[1][2] = 0.0f;
		baseTCoords[2][0] = 1.0f;  baseTCoords[2][1] = 1.0f;
		baseVCoords[2][0] = 1.0f;  baseVCoords[2][1] = 1.0f; baseVCoords[2][2] = 0.0f;
		baseTCoords[3][0] = 0.0f;  baseTCoords[3][1] = 1.0f;
		baseVCoords[3][0] = -1.0f; baseVCoords[3][1] = 1.0f; baseVCoords[3][2] = 0.0f;

		float segmentsPerTexture = (float)NUM_SEGMENTS / TEXTURE_REPEATS;
		int j = 0;
		for(int x = 0; x < NUM_SEGMENTS; x++)
		{
			for(int y = 0; y < NUM_SEGMENTS; y++)
			{
				for(int i = 0; i < 4; i++)
				{
					mLandscape->segments[j].vcoords[i][0] = 200.0f * ((x - NUM_SEGMENTS/2) * 2 + baseVCoords[i][0]) / NUM_SEGMENTS;
					mLandscape->segments[j].vcoords[i][1] = 200.0f * ((y - NUM_SEGMENTS/2) * 2 + baseVCoords[i][1]) / NUM_SEGMENTS;
					mLandscape->segments[j].vcoords[i][2] = 10.0f * getPointHeight(mLandscape->segments[j].vcoords[i][0], mLandscape->segments[j].vcoords[i][1]);

					mLandscape->segments[j].tcoords[i][0] = ((x % TEXTURE_REPEATS) / segmentsPerTexture) + ((1.0f / segmentsPerTexture) * baseTCoords[i][0]); //* 0.8f;
					mLandscape->segments[j].tcoords[i][1] = ((y % TEXTURE_REPEATS) / segmentsPerTexture) + ((1.0f / segmentsPerTexture) * baseTCoords[i][1]); //* 0.8f;
				}
				vector plane[3];
				plane[0].x = mLandscape->segments[j].vcoords[0][0];
				plane[0].y = mLandscape->segments[j].vcoords[0][1];
				plane[0].z = mLandscape->segments[j].vcoords[0][2];
				for(int i = 0; i < 2; i++)
				{
					if(i == 0)
					{
						plane[1].x = mLandscape->segments[j].vcoords[1][0];
						plane[1].y = mLandscape->segments[j].vcoords[1][1];
						plane[1].z = mLandscape->segments[j].vcoords[1][2];

						plane[2].x = mLandscape->segments[j].vcoords[2][0];
						plane[2].y = mLandscape->segments[j].vcoords[2][1];
						plane[2].z = mLandscape->segments[j].vcoords[2][2];
					}
					else
					{
						plane[1].x = mLandscape->segments[j].vcoords[2][0];
						plane[1].y = mLandscape->segments[j].vcoords[2][1];
						plane[1].z = mLandscape->segments[j].vcoords[2][2];

						plane[2].x = mLandscape->segments[j].vcoords[3][0];
						plane[2].y = mLandscape->segments[j].vcoords[3][1];
						plane[2].z = mLandscape->segments[j].vcoords[3][2];
					}
					vector planeVector;
					planeVector.x = (plane[0].x + plane[1].x + plane[2].x) / 3;
					planeVector.y = (plane[0].y + plane[1].y + plane[2].y) / 3;
					planeVector.z = (plane[0].z + plane[1].z + plane[2].z) / 3;

					mLandscape->segments[j].distance[i] = sqrt(planeVector.x*planeVector.x + planeVector.y*planeVector.y + planeVector.z*planeVector.z);

					vector v1, v2;
					v1.x = plane[1].x - plane[0].x;
					v1.y = plane[1].y - plane[0].y;
					v1.z = plane[1].z - plane[0].z;

					v2.x = plane[2].x - plane[1].x;
					v2.y = plane[2].y - plane[1].y;
					v2.z = plane[2].z - plane[1].z;

					vector faceVector;
					faceVector.x = v1.y*v2.z - v1.z*v2.y;
					faceVector.y = v1.z*v2.x - v1.x*v2.z;
					faceVector.z = v1.x*v2.y - v1.y*v2.x;

					float vLength = sqrt(faceVector.x*faceVector.x + faceVector.y*faceVector.y + faceVector.z*faceVector.z);

					mLandscape->segments[j].normalVector[i].x = faceVector.x / vLength;
					mLandscape->segments[j].normalVector[i].y = faceVector.y / vLength;
					mLandscape->segments[j].normalVector[i].z = faceVector.z / vLength;
				}
				j++;
			}
		}
	}

	float getPointHeight(float x, float y)
	{
		float freq = 0.03;
		float value = (cos(y*freq*2*M_PI) + cos(x*freq*2*M_PI))/2;
		return (value>0)?value:0;
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

		mCamera->facing = mFacing;
	}

	void runTimerEvent()
	{
		//Execute only if the screen is active
		if(true)
		{
			//Get the current system time
			int currentTime = maGetMilliSecondCount();
			float period = (currentTime-mPrevTime)/1000.0f;
			calculateAcceleration(period);
			calculatePosition(period);
			//Calculate and draw the positions for the new frame
			checkCollision();
			mRenderer.draw();
			mSecondsSinceLastUpdate += period;
			if(mSecondsSinceLastUpdate > LABEL_UPDATE_PER)
			{
				mSecondsSinceLastUpdate = 0;
				char buffer[256];
				sprintf(buffer,
				" Position - x:%f, y:%f, z:%f\n Speed - x:%f, y:%f, z:%f\n Absolute speed:%f, altitude:%f\n Segment - x:%d, y:%d, x:%4.5f, y:%4.5f, z:%4.5f",
						mPosition.x,mPosition.y,mPosition.z,mVelocity.x,mVelocity.y,mVelocity.z,mAbsSpeed,mAltitude,mX,mY,mNormal.x,mNormal.y,mNormal.z);
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
		mAcceleration.y = -mFacing.y * enginePower;
		mAcceleration.z = -mFacing.z * enginePower;
	}

	void calculatePosition(float period)
	{
		mVelocity.x += (mGravity.x + mAcceleration.x) * period;
		mVelocity.y += (mGravity.y + mAcceleration.y) * period;
		mVelocity.z += (mGravity.z + mAcceleration.z) * period;

		mAbsSpeed = sqrt(mVelocity.x*mVelocity.x + mVelocity.y*mVelocity.y + mVelocity.z*mVelocity.z);

		if(mAbsSpeed > MAX_SPEED)
		{
			mVelocity.x = MAX_SPEED * mVelocity.x/mAbsSpeed;
			mVelocity.y = MAX_SPEED * mVelocity.y/mAbsSpeed;
			mVelocity.z = MAX_SPEED * mVelocity.z/mAbsSpeed;
			mAbsSpeed = sqrt(mVelocity.x*mVelocity.x + mVelocity.y*mVelocity.y + mVelocity.z*mVelocity.z);
		}

		mPosition.x += mVelocity.x * period;
		mPosition.y += mVelocity.y * period;
		mPosition.z += mVelocity.z * period;

		mCamera->position = mPosition;
	}

	void checkCollision()
	{
		for(int i = 0; i < mLandscape->numSegments; i++)
		{
			landSegment* segment = &(mLandscape->segments[i]);
			if(	segment->vcoords[0][0] < mPosition.x &&
				segment->vcoords[2][0] > mPosition.x &&
				segment->vcoords[0][1] < mPosition.y &&
				segment->vcoords[2][1] > mPosition.y)
			{
				float dx1 = segment->vcoords[1][0] - mPosition.x;
				float dy1 = segment->vcoords[1][1] - mPosition.y;
				float dx3 = segment->vcoords[3][0] - mPosition.x;
				float dy3 = segment->vcoords[3][1] - mPosition.y;

				float d1 = dx1 * dx1 + dy1 * dy1;
				float d3 = dx3 * dx3 + dy3 * dy3;
				int side;
				if(d1 < d3)
				{
					side = 0;
				}
				else
				{
					side = 1;
				}

				mNormal = segment->normalVector[side];
				float D = segment->distance[side];

				float dot = mPosition.x * mNormal.x + mPosition.y * mNormal.y + mPosition.z * mNormal.z;
				mAltitude = dot - D;
				if(mAltitude < 5.0f)
				{
					if(		abs(mFacing.x + mNormal.x) < LANDING_DEVIATION &&
							abs(mFacing.y + mNormal.y) < LANDING_DEVIATION &&
							abs(mFacing.z + mNormal.z) < LANDING_DEVIATION &&
							mAbsSpeed < LANDING_SPEED
							)
					{
						maPanic(0,"You have landed successfully!");
					}
					else
					{
						maPanic(0,"You crashed and burned on the cold Lunar surface.");
					}
				}
			}
		}
	}

	float abs(float x)
	{
		return (x>0)?x:-x;
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
    float mAltitude;
    float mSecondsSinceLastUpdate;
    float mAbsSpeed;
    int mPrevTime;

    landscape *mLandscape;
    vector mVelocity;
    vector mPosition;
    vector mGravity;
    vector mFacing;
    vector mAcceleration;
    vector mNormal;
    bool mEnginesRunning;
    LowPassFilter mFilter;
    MobileLua::LuaEngine mLua;
    String mLocalPath;
    Renderer mRenderer;
    camera *mCamera;

    BundleDownloader *mDownloader;
};

/**
 * Main function that is called when the program starts.
 */
extern "C" int MAMain()
{
	Moblet::run(new NativeUIMoblet());
	return 0;
}
