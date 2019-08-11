/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * object.cpp - Base object
 */

#include <libcamera/object.h>

#include <libcamera/signal.h>

#include "log.h"
#include "message.h"
#include "thread.h"
#include "utils.h"

/**
 * \file object.h
 * \brief Base object to support automatic signal disconnection
 */

namespace libcamera {

/**
 * \class Object
 * \brief Base object to support automatic signal disconnection
 *
 * The Object class simplifies signal/slot handling for classes implementing
 * slots. By inheriting from Object, an object is automatically disconnected
 * from all connected signals when it gets destroyed.
 *
 * Object instances are bound to the thread in which they're created. When a
 * message is posted to an object, its handler will run in the object's thread.
 * This allows implementing easy message passing between threads by inheriting
 * from the Object class.
 *
 * Object slots connected to signals will also run in the context of the
 * object's thread, regardless of whether the signal is emitted in the same or
 * in another thread.
 *
 * \sa Message, Signal, Thread
 */

Object::Object()
	: pendingMessages_(0)
{
	thread_ = Thread::current();
}

Object::~Object()
{
	for (SignalBase *signal : signals_)
		signal->disconnect(this);

	if (pendingMessages_)
		thread()->removeMessages(this);
}

/**
 * \brief Post a message to the object's thread
 * \param[in] msg The message
 *
 * This method posts the message \a msg to the message queue of the object's
 * thread, to be delivered to the object through the message() method in the
 * context of its thread. Message ownership is passed to the thread, and the
 * message will be deleted after being delivered.
 *
 * Messages are delivered through the thread's event loop. If the thread is not
 * running its event loop the message will not be delivered until the event
 * loop gets started.
 */
void Object::postMessage(std::unique_ptr<Message> msg)
{
	thread()->postMessage(std::move(msg), this);
}

/**
 * \brief Message handler for the object
 * \param[in] msg The message
 *
 * This virtual method receives messages for the object. It is called in the
 * context of the object's thread, and can be overridden to process custom
 * messages. The parent Object::message() method shall be called for any
 * message not handled by the override method.
 *
 * The message \a msg is valid only for the duration of the call, no reference
 * to it shall be kept after this method returns.
 */
void Object::message(Message *msg)
{
	switch (msg->type()) {
	case Message::InvokeMessage: {
		InvokeMessage *iMsg = static_cast<InvokeMessage *>(msg);
		iMsg->invoke();
		break;
	}

	default:
		break;
	}
}

/**
 * \fn void Object::invokeMethod(void (T::*func)(Args...), Args... args)
 * \brief Invoke a method asynchronously on an Object instance
 * \param[in] func The object method to invoke
 * \param[in] args The method arguments
 *
 * This method invokes the member method \a func when control returns to the
 * event loop of the object's thread. The method is executed in the object's
 * thread with arguments \a args.
 *
 * Arguments \a args passed by value or reference are copied, while pointers
 * are passed untouched. The caller shall ensure that any pointer argument
 * remains valid until the method is invoked.
 */

void Object::invokeMethod(BoundMethodBase *method, void *args)
{
	std::unique_ptr<Message> msg =
		utils::make_unique<InvokeMessage>(method, args, true);
	postMessage(std::move(msg));
}

/**
 * \fn Object::thread()
 * \brief Retrieve the thread the object is bound to
 * \return The thread the object is bound to
 */

/**
 * \brief Move the object to a different thread
 * \param[in] thread The target thread
 *
 * This method moves the object from the current thread to the new \a thread.
 * It shall be called from the thread in which the object currently lives,
 * otherwise the behaviour is undefined.
 *
 * Before the object is moved, a Message::ThreadMoveMessage message is sent to
 * it. The message() method can be reimplement in derived classes to be notified
 * of the upcoming thread move and perform any required processing.
 */
void Object::moveToThread(Thread *thread)
{
	ASSERT(Thread::current() == thread_);

	if (thread_ == thread)
		return;

	notifyThreadMove();

	thread->moveObject(this);
}

void Object::notifyThreadMove()
{
	Message msg(Message::ThreadMoveMessage);
	message(&msg);
}

void Object::connect(SignalBase *signal)
{
	signals_.push_back(signal);
}

void Object::disconnect(SignalBase *signal)
{
	for (auto iter = signals_.begin(); iter != signals_.end(); ) {
		if (*iter == signal)
			iter = signals_.erase(iter);
		else
			iter++;
	}
}

}; /* namespace libcamera */
