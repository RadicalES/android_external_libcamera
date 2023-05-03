/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018, Google Inc.
 *
 * camera_manager.h - Camera management
 */

#include "libcamera/internal/camera_manager.h"

#include <libcamera/base/log.h>
#include <libcamera/base/utils.h>

#include <libcamera/camera.h>
#include <libcamera/property_ids.h>

#include "libcamera/internal/camera.h"
#include "libcamera/internal/device_enumerator.h"
#include "libcamera/internal/pipeline_handler.h"

/**
 * \file libcamera/camera_manager.h
 * \brief The camera manager
 */

/**
 * \file libcamera/internal/camera_manager.h
 * \brief Internal camera manager support
 */

/**
 * \brief Top-level libcamera namespace
 */
namespace libcamera {

LOG_DEFINE_CATEGORY(Camera)

CameraManager::Private::Private()
	: initialized_(false)
{
}

int CameraManager::Private::start()
{
	int status;

	/* Start the thread and wait for initialization to complete. */
	Thread::start();

	{
		MutexLocker locker(mutex_);
		cv_.wait(locker, [&]() LIBCAMERA_TSA_REQUIRES(mutex_) {
			return initialized_;
		});
		status = status_;
	}

	/* If a failure happened during initialization, stop the thread. */
	if (status < 0) {
		exit();
		wait();
		return status;
	}

	return 0;
}

void CameraManager::Private::run()
{
	LOG(Camera, Debug) << "Starting camera manager";

	int ret = init();

	mutex_.lock();
	status_ = ret;
	initialized_ = true;
	mutex_.unlock();
	cv_.notify_one();

	if (ret < 0)
		return;

	/* Now start processing events and messages. */
	exec();

	cleanup();
}

int CameraManager::Private::init()
{
	enumerator_ = DeviceEnumerator::create();
	if (!enumerator_ || enumerator_->enumerate())
		return -ENODEV;

	createPipelineHandlers();
	enumerator_->devicesAdded.connect(this, &Private::createPipelineHandlers);

	return 0;
}

void CameraManager::Private::createPipelineHandlers()
{
	CameraManager *const o = LIBCAMERA_O_PTR();

	/*
	 * \todo Try to read handlers and order from configuration
	 * file and only fallback on all handlers if there is no
	 * configuration file.
	 */
	const std::vector<PipelineHandlerFactoryBase *> &factories =
		PipelineHandlerFactoryBase::factories();

	for (const PipelineHandlerFactoryBase *factory : factories) {
		LOG(Camera, Debug)
			<< "Found registered pipeline handler '"
			<< factory->name() << "'";
		/*
		 * Try each pipeline handler until it exhaust
		 * all pipelines it can provide.
		 */
		while (1) {
			std::shared_ptr<PipelineHandler> pipe = factory->create(o);
			if (!pipe->match(enumerator_.get()))
				break;

			LOG(Camera, Debug)
				<< "Pipeline handler \"" << factory->name()
				<< "\" matched";
		}
	}
}

void CameraManager::Private::cleanup()
{
	enumerator_->devicesAdded.disconnect(this);

	/*
	 * Release all references to cameras to ensure they all get destroyed
	 * before the device enumerator deletes the media devices. Cameras are
	 * destroyed via Object::deleteLater() API, hence we need to explicitly
	 * process deletion requests from the thread's message queue as the event
	 * loop is not in action here.
	 */
	{
		MutexLocker locker(mutex_);
		cameras_.clear();
	}

	dispatchMessages(Message::Type::DeferredDelete);

	enumerator_.reset(nullptr);
}

/**
 * \brief Add a camera to the camera manager
 * \param[in] camera The camera to be added
 *
 * This function is called by pipeline handlers to register the cameras they
 * handle with the camera manager. Registered cameras are immediately made
 * available to the system.
 *
 * Device numbers from the SystemDevices property are used by the V4L2
 * compatibility layer to map V4L2 device nodes to Camera instances.
 *
 * \context This function shall be called from the CameraManager thread.
 */
void CameraManager::Private::addCamera(std::shared_ptr<Camera> camera)
{
	ASSERT(Thread::current() == this);

	MutexLocker locker(mutex_);

	for (const std::shared_ptr<Camera> &c : cameras_) {
		if (c->id() == camera->id()) {
			LOG(Camera, Fatal)
				<< "Trying to register a camera with a duplicated ID '"
				<< camera->id() << "'";
			return;
		}
	}

	auto devnums = camera->properties()
			       .get(properties::SystemDevices)
			       .value_or(Span<int64_t>{});

	cameras_.push_back(std::move(camera));

	unsigned int index = cameras_.size() - 1;
	for (dev_t devnum : devnums)
		camerasByDevnum_[devnum] = cameras_[index];

	/* Report the addition to the public signal */
	CameraManager *const o = LIBCAMERA_O_PTR();
	o->cameraAdded.emit(cameras_[index]);
}

/**
 * \brief Remove a camera from the camera manager
 * \param[in] camera The camera to be removed
 *
 * This function is called by pipeline handlers to unregister cameras from the
 * camera manager. Unregistered cameras won't be reported anymore by the
 * cameras() and get() calls, but references may still exist in applications.
 *
 * \context This function shall be called from the CameraManager thread.
 */
void CameraManager::Private::removeCamera(std::shared_ptr<Camera> camera)
{
	ASSERT(Thread::current() == this);

	MutexLocker locker(mutex_);

	auto iter = std::find_if(cameras_.begin(), cameras_.end(),
				 [camera](std::shared_ptr<Camera> &c) {
					 return c.get() == camera.get();
				 });
	if (iter == cameras_.end())
		return;

	LOG(Camera, Debug)
		<< "Unregistering camera '" << camera->id() << "'";

	auto iter_d = std::find_if(camerasByDevnum_.begin(), camerasByDevnum_.end(),
				   [camera](const std::pair<dev_t, std::weak_ptr<Camera>> &p) {
					   return p.second.lock().get() == camera.get();
				   });
	if (iter_d != camerasByDevnum_.end())
		camerasByDevnum_.erase(iter_d);

	cameras_.erase(iter);

	/* Report the removal to the public signal */
	CameraManager *const o = LIBCAMERA_O_PTR();
	o->cameraRemoved.emit(camera);
}

/**
 * \class CameraManager
 * \brief Provide access and manage all cameras in the system
 *
 * The camera manager is the entry point to libcamera. It enumerates devices,
 * associates them with pipeline managers, and provides access to the cameras
 * in the system to applications. The manager owns all Camera objects and
 * handles hot-plugging and hot-unplugging to manage the lifetime of cameras.
 *
 * To interact with libcamera, an application starts by creating a camera
 * manager instance. Only a single instance of the camera manager may exist at
 * a time. Attempting to create a second instance without first deleting the
 * existing instance results in undefined behaviour.
 *
 * The manager is initially stopped, and shall be started with start(). This
 * will enumerate all the cameras present in the system, which can then be
 * listed with list() and retrieved with get().
 *
 * Cameras are shared through std::shared_ptr<>, ensuring that a camera will
 * stay valid until the last reference is released without requiring any special
 * action from the application. Once the application has released all the
 * references it held to cameras, the camera manager can be stopped with
 * stop().
 */

CameraManager *CameraManager::self_ = nullptr;

CameraManager::CameraManager()
	: Extensible(std::make_unique<CameraManager::Private>())
{
	if (self_)
		LOG(Camera, Fatal)
			<< "Multiple CameraManager objects are not allowed";

	self_ = this;
}

/**
 * \brief Destroy the camera manager
 *
 * Destroying the camera manager stops it if it is currently running.
 */
CameraManager::~CameraManager()
{
	stop();

	self_ = nullptr;
}

/**
 * \brief Start the camera manager
 *
 * Start the camera manager and enumerate all devices in the system. Once
 * the start has been confirmed the user is free to list and otherwise
 * interact with cameras in the system until either the camera manager
 * is stopped or the camera is unplugged from the system.
 *
 * \return 0 on success or a negative error code otherwise
 */
int CameraManager::start()
{
	LOG(Camera, Info) << "libcamera " << version_;

	int ret = _d()->start();
	if (ret)
		LOG(Camera, Error) << "Failed to start camera manager: "
				   << strerror(-ret);

	return ret;
}

/**
 * \brief Stop the camera manager
 *
 * Before stopping the camera manager the caller is responsible for making
 * sure all cameras provided by the manager are returned to the manager.
 *
 * After the manager has been stopped no resource provided by the camera
 * manager should be consider valid or functional even if they for one
 * reason or another have yet to be deleted.
 */
void CameraManager::stop()
{
	Private *const d = _d();
	d->exit();
	d->wait();
}

/**
 * \fn CameraManager::cameras()
 * \brief Retrieve all available cameras
 *
 * Before calling this function the caller is responsible for ensuring that
 * the camera manager is running.
 *
 * \context This function is \threadsafe.
 *
 * \return List of all available cameras
 */
std::vector<std::shared_ptr<Camera>> CameraManager::cameras() const
{
	const Private *const d = _d();

	MutexLocker locker(d->mutex_);

	return d->cameras_;
}

/**
 * \brief Get a camera based on ID
 * \param[in] id ID of camera to get
 *
 * Before calling this function the caller is responsible for ensuring that
 * the camera manager is running.
 *
 * \context This function is \threadsafe.
 *
 * \return Shared pointer to Camera object or nullptr if camera not found
 */
std::shared_ptr<Camera> CameraManager::get(const std::string &id)
{
	Private *const d = _d();

	MutexLocker locker(d->mutex_);

	for (std::shared_ptr<Camera> camera : d->cameras_) {
		if (camera->id() == id)
			return camera;
	}

	return nullptr;
}

/**
 * \brief Retrieve a camera based on device number
 * \param[in] devnum Device number of camera to get
 *
 * This function is meant solely for the use of the V4L2 compatibility
 * layer, to map device nodes to Camera instances. Applications shall
 * not use it and shall instead retrieve cameras by name.
 *
 * Before calling this function the caller is responsible for ensuring that
 * the camera manager is running.
 *
 * \context This function is \threadsafe.
 *
 * \return Shared pointer to Camera object, which is empty if the camera is
 * not found
 */
std::shared_ptr<Camera> CameraManager::get(dev_t devnum)
{
	Private *const d = _d();

	MutexLocker locker(d->mutex_);

	auto iter = d->camerasByDevnum_.find(devnum);
	if (iter == d->camerasByDevnum_.end())
		return nullptr;

	return iter->second.lock();
}

/**
 * \var CameraManager::cameraAdded
 * \brief Notify of a new camera added to the system
 *
 * This signal is emitted when a new camera is detected and successfully handled
 * by the camera manager. The notification occurs alike for cameras detected
 * when the manager is started with start() or when new cameras are later
 * connected to the system. When the signal is emitted the new camera is already
 * available from the list of cameras().
 *
 * The signal is emitted from the CameraManager thread. Applications shall
 * minimize the time spent in the signal handler and shall in particular not
 * perform any blocking operation.
 */

/**
 * \var CameraManager::cameraRemoved
 * \brief Notify of a new camera removed from the system
 *
 * This signal is emitted when a camera is removed from the system. When the
 * signal is emitted the camera is not available from the list of cameras()
 * anymore.
 *
 * The signal is emitted from the CameraManager thread. Applications shall
 * minimize the time spent in the signal handler and shall in particular not
 * perform any blocking operation.
 */

/**
 * \fn const std::string &CameraManager::version()
 * \brief Retrieve the libcamera version string
 * \context This function is \a threadsafe.
 * \return The libcamera version string
 */

} /* namespace libcamera */
