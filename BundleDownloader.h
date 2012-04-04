/*
 * BundleDownloader.h
 *
 *  Created on: Apr 4, 2012
 *      Author: iraklis
 */

#ifndef BUNDLEDOWNLOADER_H_
#define BUNDLEDOWNLOADER_H_

#include <MAUtil/Environment.h>
#include <MAUtil/Connection.h>
#include <wchar.h>
#include <MAUtil/Downloader.h>

using namespace MAUtil;

class BundleListener
{
public:
	virtual void bundleDownloaded(MAHandle data);
};

class BundleDownloader: public TextBoxListener, ConnectionListener, public DownloadListener
{
public:
	BundleDownloader(BundleListener *bl);

	void textBoxClosed(int result, int textLength);

	void connectFinished(Connection *conn, int result);

	void connRecvFinished(Connection *conn, int result);

	void downloadBundle();

	void downloadCancelled(Downloader* downloader);

	void error(Downloader* downloader, int code);

	void finishedDownloading(Downloader* downloader, MAHandle data);

private:
	BundleListener *mBundleListener;
    wchar mWServerAddress[128];
    char mServerAddress[128];
	Connection mSocket;
	bool hasPage;

	/**
	 * Buffer for TCP messages
	 */
	char mBuffer[1024];

	/**
	 * Buffer for the bundle address
	 */
	char mBundleAddress[256];

	Downloader *mDownloader;
	MAHandle mResourceFile;
};


#endif /* BUNDLEDOWNLOADER_H_ */
