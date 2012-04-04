/*
 * BundleDownloader.cpp
 *
 *  Created on: Apr 4, 2012
 *      Author: iraklis
 */

#include <ma.h>
#include <mavsprintf.h>
#include <conprint.h>
#include "BundleDownloader.h"

BundleDownloader::BundleDownloader(BundleListener *bl):mSocket(this)
{
	mBundleListener = bl;
	maTextBox(L"Server IP:",L"localhost", mWServerAddress, 128, 0);
	Environment::getEnvironment().addTextBoxListener(this);
}

void BundleDownloader::textBoxClosed(int result, int textLength)
{
	if(result == MA_TB_RES_OK)
	{
		mDownloader = new Downloader();
		mDownloader->addDownloadListener(this);

		//User tries to connect, reset the socket and start a new connection
		mSocket.close();
		wcstombs(mServerAddress, mWServerAddress, 128);
		//Add the port number to the IP
		sprintf(mBuffer,"socket://%s:7000", mServerAddress);
		lprintfln(mBuffer);
		mSocket.connect(mBuffer);
	}
}

//The socket->connect() operation has finished
void BundleDownloader::connectFinished(Connection *conn, int result)
{
	printf("connection result: %d\n", result);
	if(result > 0)
	{
		mSocket.recv(mBuffer,1024);
	}
	else
	{
		//showConErrorMessage(result);
	}
}

//We received a TCP message from the server
void BundleDownloader::connRecvFinished(Connection *conn, int result)
{
	lprintfln("recv result: %d\n", result);
	if(result > 0)
	{
		//Null terminate the string message (it's a URL of the .bin bundle)
		mBuffer[result] = '\0';
		sprintf(mBundleAddress,"http://%ls:8282%s", mServerAddress, mBuffer);
		lprintfln("FileURL:%s\n",mBundleAddress);
		//Reset the app environment (destroy widgets, stop sensors)
        //freeHardware();
        downloadBundle();
		//Set the socket to receive the next TCP message
		mSocket.recv(mBuffer, 1024);


	}
	else
	{
		printf("connRecvFinished result %d", result);
		//showConErrorMessage(result);

	}
}

void BundleDownloader::downloadBundle()
{
	//Prepare a reciever for the download
	mResourceFile = maCreatePlaceholder();
	//Start the bundle download
	if(mDownloader->isDownloading())
	{
		mDownloader->cancelDownloading();
	}
	int res = mDownloader->beginDownloading(mBundleAddress, mResourceFile);
	if(res > 0)
	{
		printf("Downloading Started with %d\n", res);
	}
	else
	{
		//showConErrorMessage(res);
	}
}


/**
 * Called when a download operation is canceled
 * @param downloader The downloader that was canceled
 */
void BundleDownloader::downloadCancelled(Downloader* downloader)
{
    printf("Cancelled");
}

/**
 * Method displays error code in case of error in downloading.
 * @param downloader The downloader that got the error
 * @param code The error code that was returned
 */
void BundleDownloader::error(Downloader* downloader, int code)
{
    printf("Error: %d", code);
    //showConErrorMessage(code);
}

/**
 * Called when the download is complete
 * @param downloader The downloader who finished it's operation
 * @param data A handle to the data that was downloaded
 */
void BundleDownloader::finishedDownloading(Downloader* downloader, MAHandle data)
{
    lprintfln("Completed download");
    //extract the file System
    mBundleListener->bundleDownloaded(data);
    maDestroyPlaceholder(mResourceFile);
    //loadSavedApp();
}
