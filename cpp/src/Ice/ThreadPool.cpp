// **********************************************************************
//
// Copyright (c) 2003-2004 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <Ice/ThreadPool.h>
#include <Ice/EventHandler.h>
#include <Ice/Network.h>
#include <Ice/LocalException.h>
#include <Ice/Instance.h>
#include <Ice/LoggerUtil.h>
#include <Ice/Functional.h>
#include <Ice/Protocol.h>
#include <Ice/ObjectAdapterFactory.h>
#include <Ice/Properties.h>

using namespace std;
using namespace Ice;
using namespace IceInternal;

void IceInternal::incRef(ThreadPool* p) { p->__incRef(); }
void IceInternal::decRef(ThreadPool* p) { p->__decRef(); }

IceInternal::ThreadPool::ThreadPool(const InstancePtr& instance, const string& prefix, int timeout) :
    _instance(instance),
    _destroyed(false),
    _prefix(prefix),
    _lastFd(INVALID_SOCKET),
    _timeout(timeout),
    _size(0),
    _sizeMax(0),
    _sizeWarn(0),
    _stackSize(0),
    _running(0),
    _inUse(0),
    _load(0),
    _promote(true),
    _warnUdp(_instance->properties()->getPropertyAsInt("Ice.Warn.Datagrams") > 0)
{
    SOCKET fds[2];
    createPipe(fds);
    _fdIntrRead = fds[0];
    _fdIntrWrite = fds[1];
    setBlock(_fdIntrRead, false);

    FD_ZERO(&_fdSet);
    FD_SET(_fdIntrRead, &_fdSet);
    _maxFd = _fdIntrRead;
    _minFd = _fdIntrRead;

    //
    // We use just one thread as the default. This is the fastest
    // possible setting, still allows one level of nesting, and
    // doesn't require to make the servants thread safe.
    //
    int size = _instance->properties()->getPropertyAsIntWithDefault(_prefix + ".Size", 1);
    if(size < 1)
    {
	size = 1;
    }
    
    int sizeMax = _instance->properties()->getPropertyAsIntWithDefault(_prefix + ".SizeMax", size);
    if(sizeMax < size)
    {
	sizeMax = size;
    }		
    
    int sizeWarn = _instance->properties()->getPropertyAsIntWithDefault(_prefix + ".SizeWarn", sizeMax * 80 / 100);

    const_cast<int&>(_size) = size;
    const_cast<int&>(_sizeMax) = sizeMax;
    const_cast<int&>(_sizeWarn) = sizeWarn;

    int stackSize = _instance->properties()->getPropertyAsIntWithDefault(_prefix + ".StackSize", 0);
    if(stackSize < 0)
    {
	stackSize = 0;
    }
    const_cast<size_t&>(_stackSize) = static_cast<size_t>(stackSize);

    __setNoDelete(true);
    try
    {
	for(int i = 0 ; i < _size ; ++i)
	{
	    IceUtil::ThreadPtr thread = new EventHandlerThread(this);
	    _threads.push_back(thread->start(_stackSize));
	    ++_running;
	}
    }
    catch(const IceUtil::Exception& ex)
    {
	{
	    Error out(_instance->logger());
	    out << "cannot create thread for `" << _prefix << "':\n" << ex;
	}

	destroy();
	joinWithAllThreads();
	__setNoDelete(false);
	throw;
    }
    catch(...)
    {
	__setNoDelete(false);
	throw;
    }
    __setNoDelete(false);
}

IceInternal::ThreadPool::~ThreadPool()
{
    assert(_destroyed);

//
// For some unknown reason, closing sockets below fails under WIN32,
// with an exception that indicates that WSACleanup() would have been
// called (error code WSANOTINITIALISED). However, traces show that
// this is not the case, i.e., at least Ice doesn't call WSACleanup()
// before the code below.
//
#ifndef _WIN32
    try
    {
	closeSocket(_fdIntrWrite);
	closeSocket(_fdIntrRead);
    }
    catch(const LocalException& ex)
    {
	Error out(_instance->logger());
	out << "exception in `" << _prefix << "' while calling closeSocket():\n" << ex;
    }
#endif
}

void
IceInternal::ThreadPool::destroy()
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
    assert(!_destroyed);
    assert(_handlerMap.empty());
    assert(_changes.empty());
    _destroyed = true;
    setInterrupt();
}

void
IceInternal::ThreadPool::_register(SOCKET fd, const EventHandlerPtr& handler)
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
    assert(!_destroyed);
    _changes.push_back(make_pair(fd, handler));
    setInterrupt();
}

void
IceInternal::ThreadPool::unregister(SOCKET fd)
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
    assert(!_destroyed);
    _changes.push_back(make_pair(fd, EventHandlerPtr(0)));
    setInterrupt();
}

void
IceInternal::ThreadPool::promoteFollower()
{
    if(_sizeMax > 1)
    {
	IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);

	assert(!_promote);
	_promote = true;
	notify();

	if(!_destroyed)
	{
	    assert(_inUse >= 0);
	    ++_inUse;
	    
	    if(_inUse == _sizeWarn)
	    {
		Warning out(_instance->logger());
		out << "thread pool `" << _prefix << "' is running low on threads\n"
		    << "Size=" << _size << ", " << "SizeMax=" << _sizeMax << ", " << "SizeWarn=" << _sizeWarn;
	    }
	    
	    assert(_inUse <= _running);
	    if(_inUse < _sizeMax && _inUse == _running)
	    {
		try
		{
		    IceUtil::ThreadPtr thread = new EventHandlerThread(this);
		    _threads.push_back(thread->start(_stackSize));
		    ++_running;
		}
		catch(const IceUtil::Exception& ex)
		{
		    Error out(_instance->logger());
		    out << "cannot create thread for `" << _prefix << "':\n" << ex;
		}
	    }
	}
    }
}

void
IceInternal::ThreadPool::joinWithAllThreads()
{
    //
    // _threads is immutable after destroy() has been called,
    // therefore no synchronization is needed. (Synchronization
    // wouldn't be possible here anyway, because otherwise the other
    // threads would never terminate.)
    //
    assert(_destroyed);
#if defined(_MSC_VER) && _MSC_VER <= 1200 // The mem_fun_ref below does not work with VC++ 6.0
    for(vector<IceUtil::ThreadControl>::iterator p = _threads.begin(); p != _threads.end(); ++p)
    {
	p->join();
    }
#else
    for_each(_threads.begin(), _threads.end(), mem_fun_ref(&IceUtil::ThreadControl::join));
#endif
}

string
IceInternal::ThreadPool::prefix() const
{
    return _prefix;
}

void
IceInternal::ThreadPool::clearInterrupt()
{
    char c;

repeat:

#ifdef _WIN32
    if(::recv(_fdIntrRead, &c, 1, 0) == SOCKET_ERROR)
    {
	if(interrupted())
	{
	    goto repeat;
	}

	SocketException ex(__FILE__, __LINE__);
	ex.error = getSocketErrno();
	throw ex;
    }
#else
    if(::read(_fdIntrRead, &c, 1) == -1)
    {
	if(interrupted())
	{
	    goto repeat;
	}

	SyscallException ex(__FILE__, __LINE__);
	ex.error = getSystemErrno();
	throw ex;
    }
#endif
}

void
IceInternal::ThreadPool::setInterrupt()
{
    char c = 0;

repeat:

#ifdef _WIN32
    if(::send(_fdIntrWrite, &c, 1, 0) == SOCKET_ERROR)
    {
	if(interrupted())
	{
	    goto repeat;
	}

	SocketException ex(__FILE__, __LINE__);
	ex.error = getSocketErrno();
	throw ex;
    }
#else
    if(::write(_fdIntrWrite, &c, 1) == -1)
    {
	if(interrupted())
	{
	    goto repeat;
	}

	SyscallException ex(__FILE__, __LINE__);
	ex.error = getSystemErrno();
	throw ex;
    }
#endif
}

bool
IceInternal::ThreadPool::run()
{
    ThreadPoolPtr self = this;

    if(_sizeMax > 1)
    {
	IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);

	while(!_promote)
	{
	    wait();
	}

	_promote = false;
    }

    while(true)
    {
	fd_set fdSet;
	memcpy(&fdSet, &_fdSet, sizeof(fd_set));
	int ret;
	if(_timeout > 0)
	{
	    struct timeval tv;
	    tv.tv_sec = _timeout;
	    tv.tv_usec = 0;
	    ret = ::select(_maxFd + 1, &fdSet, 0, 0, &tv);
	}
	else
	{
	    ret = ::select(_maxFd + 1, &fdSet, 0, 0, 0);
	}
	
	if(ret == SOCKET_ERROR)
	{
	    if(interrupted())
	    {
		continue;
	    }
	    
	    SocketException ex(__FILE__, __LINE__);
	    ex.error = getSocketErrno();
	    //throw ex;
	    Error out(_instance->logger());
	    out << "exception in `" << _prefix << "':\n" << ex; 
	    continue;
	}
	
	EventHandlerPtr handler;
	bool finished = false;
	bool shutdown = false;

	{
	    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
	    
	    if(ret == 0) // We initiate a shutdown if there is a thread pool timeout.
	    {
		assert(_timeout > 0);
		_timeout = 0;
		shutdown = true;
	    }
	    else
	    {
		if(FD_ISSET(_fdIntrRead, &fdSet))
		{
		    //
		    // There are two possiblities for an interrupt:
		    //
		    // 1. The thread pool has been destroyed.
		    //
		    // 2. An event handler was registered or unregistered.
		    //

		    //
		    // Thread pool destroyed?
		    //
		    if(_destroyed)
		    {
			//
			// Don't clear the interrupt if destroyed, so that
			// the other threads exit as well.
			//
			return true;
		    }
		    
		    clearInterrupt();
		    
		    //
		    // An event handler must have been registered or
		    // unregistered.
		    //
		    assert(!_changes.empty());
		    pair<SOCKET, EventHandlerPtr> change = _changes.front();
		    _changes.pop_front();
		    
		    if(change.second) // Addition if handler is set.
		    {
			_handlerMap.insert(change);
			FD_SET(change.first, &_fdSet);
			_maxFd = max(_maxFd, change.first);
			_minFd = min(_minFd, change.first);
			continue;
		    }
		    else // Removal if handler is not set.
		    {
			map<SOCKET, EventHandlerPtr>::iterator p = _handlerMap.find(change.first);
			assert(p != _handlerMap.end());
			handler = p->second;
			finished = true;
			_handlerMap.erase(p);
			FD_CLR(change.first, &_fdSet);
			_maxFd = _fdIntrRead;
			_minFd = _fdIntrRead;
			if(!_handlerMap.empty())
			{
			    _maxFd = max(_maxFd, (--_handlerMap.end())->first);
			    _minFd = min(_minFd, _handlerMap.begin()->first);
			}
			// Don't continue; we have to call
			// finished() on the event handler below, outside
			// the thread synchronization.
		    }
		}
		else
		{
//
// Optimization for WIN32 specific version of fd_set. Looping with a
// FD_ISSET test like for Unix is very inefficient for WIN32.
//
#ifdef _WIN32
		    //
		    // Round robin for the filedescriptors.
		    //
		    if(fdSet.fd_count == 0)
		    {
			Error out(_instance->logger());
			out << "select() in `" << _prefix << "' returned " << ret
			    << " but no filedescriptor is readable";
			continue;
		    }
		    
		    SOCKET largerFd = _maxFd + 1;
		    SOCKET smallestFd = _maxFd + 1;
		    for(u_short i = 0; i < fdSet.fd_count; ++i)
		    {
			SOCKET fd = fdSet.fd_array[i];
			assert(fd != INVALID_SOCKET);
			
			if(fd > _lastFd || _lastFd == INVALID_SOCKET)
			{
			    largerFd = min(largerFd, fd);
			}
			
			smallestFd = min(smallestFd, fd);
		    }
		    
		    if(largerFd <= _maxFd)
		    {
			assert(largerFd >= _minFd);
			_lastFd = largerFd;
		    }
		    else
		    {
			assert(smallestFd >= _minFd && smallestFd <= _maxFd);
			_lastFd = smallestFd;
		    }
#else
		    //
		    // Round robin for the filedescriptors.
		    //
		    if(_lastFd < _minFd - 1 || _lastFd == INVALID_SOCKET)
		    {
			_lastFd = _minFd - 1;
		    }
		    
		    int loops = 0;
		    do
		    {
			if(++_lastFd > _maxFd)
			{
			    ++loops;
			    _lastFd = _minFd;
			}
		    }
		    while(!FD_ISSET(_lastFd, &fdSet) && loops <= 1);
		    
		    if(loops > 1)
		    {
			Error out(_instance->logger());
			out << "select() in `" << _prefix << "' returned " << ret
			    << " but no filedescriptor is readable";
			continue;
		    }
#endif
		    
		    assert(_lastFd != _fdIntrRead);
		    
		    map<SOCKET, EventHandlerPtr>::iterator p = _handlerMap.find(_lastFd);
		    if(p == _handlerMap.end())
		    {
			Error out(_instance->logger());
			out << "filedescriptor " << _lastFd << " not registered with `" << _prefix << "'";
			continue;
		    }
		    
		    handler = p->second;
		}
	    }
	}
	
	//
	// Now we are outside the thread synchronization.
	//

	if(shutdown)
	{
	    //
	    // Initiate server shutdown.
	    //
	    ObjectAdapterFactoryPtr factory;
	    try
	    {
		factory = _instance->objectAdapterFactory();
	    }
	    catch(const Ice::CommunicatorDestroyedException&)
	    {
		continue;
	    }

	    promoteFollower();
	    factory->shutdown();

	    //
	    // No "continue", because we want shutdown to be done in
	    // its own thread from this pool. Therefore we called
	    // promoteFollower().
	    //
	}
	else
	{
	    assert(handler);
	    
	    if(finished)
	    {
		//
		// Notify a handler about it's removal from the thread
		// pool.
		//
		try
		{
		    //
		    // "self" is faster than "this", as the reference
		    // count is not modified.
		    //
		    handler->finished(self);
		}
		catch(const LocalException& ex)
		{
		    Error out(_instance->logger());
		    out << "exception in `" << _prefix << "' while calling finished():\n"
			<< ex << '\n' << handler->toString();
		}

		//
		// No "continue", because we want finished() to be
		// called in its own thread from this pool. Note that
		// this means that finished() must call
		// promoteFollower().
		//
	    }
	    else
	    {
		//
		// If the handler is "readable", try to read a
		// message.
		//
		BasicStream stream(_instance.get());
		if(handler->readable())
		{
		    try
		    {
			read(handler);
		    }
		    catch(const TimeoutException&) // Expected.
		    {
			continue;
		    }
		    catch(const DatagramLimitException&) // Expected.
		    {
			continue;
		    }
		    catch(const LocalException& ex)
		    {
			handler->exception(ex);
			continue;
		    }
		    
		    stream.swap(handler->_stream);
		    assert(stream.i == stream.b.end());
		}
	    
		//
		// Provide a new mesage to the handler.
		//
		try
		{
		    //
		    // "self" is faster than "this", as the reference
		    // count is not modified.
		    //
		    handler->message(stream, self);
		}
		catch(const LocalException& ex)
		{
		    Error out(_instance->logger());
		    out << "exception in `" << _prefix << "' while calling finished():\n"
			<< ex << '\n' << handler->toString();
		}
		
		//
		// No "continue", because we want message() to be
		// called in its own thread from this pool. Note that
		// this means that message() must call
		// promoteFollower().
		//
	    }
	}

	if(_sizeMax > 1)
	{
	    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
	    
	    if(!_destroyed)
	    {
		//
		// First we reap threads that have been destroyed before.
		//
		int sz = static_cast<int>(_threads.size());
		assert(_running <= sz);
		if(_running < sz)
		{
		    vector<IceUtil::ThreadControl>::iterator start =
			partition(_threads.begin(), _threads.end(), mem_fun_ref(&IceUtil::ThreadControl::isAlive));
#if defined(_MSC_VER) && _MSC_VER <= 1200 // The mem_fun_ref below does not work with VC++ 6.0
		    for(vector<IceUtil::ThreadControl>::iterator p = start; p != _threads.end(); ++p)
		    {
			p->join();
		    }
#else
		    for_each(start, _threads.end(), mem_fun_ref(&IceUtil::ThreadControl::join));
#endif
		    _threads.erase(start, _threads.end());
		}
		
		//
		// Now we check if this thread can be destroyed, based
		// on a load factor.
		//
		const double loadFactor = 0.05; // TODO: Configurable?
		const double oneMinusLoadFactor = 1 - loadFactor;
		_load = _load * oneMinusLoadFactor + _inUse * loadFactor;
		
		if(_running > _size)
		{
		    int load = static_cast<int>(_load + 1);
		    if(load < _running)
		    {
			assert(_inUse > 0);
			--_inUse;
			
			assert(_running > 0);
			--_running;
			
			return false;
		    }
		}
		
		assert(_inUse > 0);
		--_inUse;
	    }


	    while(!_promote)
	    {
		wait();
	    }
	    
	    _promote = false;
	}
    }
}

void
IceInternal::ThreadPool::read(const EventHandlerPtr& handler)
{
    BasicStream& stream = handler->_stream;
    
    if(stream.b.size() == 0)
    {
	stream.b.resize(headerSize);
	stream.i = stream.b.begin();
    }

    if(stream.i != stream.b.end())
    {
	handler->read(stream);
	assert(stream.i == stream.b.end());
    }
    
    ptrdiff_t pos = stream.i - stream.b.begin();
    assert(pos >= headerSize);
    stream.i = stream.b.begin();
    ByteSeq m(sizeof(magic), 0);
    stream.readBlob(m, static_cast<Int>(sizeof(magic)));
    if(!equal(m.begin(), m.end(), magic))
    {
	BadMagicException ex(__FILE__, __LINE__);
	ex.badMagic = m;
	throw ex;
    }
    Byte pMajor;
    Byte pMinor;
    stream.read(pMajor);
    stream.read(pMinor);
    if(pMajor != protocolMajor
       || static_cast<unsigned char>(pMinor) > static_cast<unsigned char>(protocolMinor))
    {
	UnsupportedProtocolException ex(__FILE__, __LINE__);
	ex.badMajor = static_cast<unsigned char>(pMajor);
	ex.badMinor = static_cast<unsigned char>(pMinor);
	ex.major = static_cast<unsigned char>(protocolMajor);
	ex.minor = static_cast<unsigned char>(protocolMinor);
	throw ex;
    }
    Byte eMajor;
    Byte eMinor;
    stream.read(eMajor);
    stream.read(eMinor);
    if(eMajor != encodingMajor
       || static_cast<unsigned char>(eMinor) > static_cast<unsigned char>(encodingMinor))
    {
	UnsupportedEncodingException ex(__FILE__, __LINE__);
	ex.badMajor = static_cast<unsigned char>(eMajor);
	ex.badMinor = static_cast<unsigned char>(eMinor);
	ex.major = static_cast<unsigned char>(encodingMajor);
	ex.minor = static_cast<unsigned char>(encodingMinor);
	throw ex;
    }
    Byte messageType;
    stream.read(messageType);
    Byte compress;
    stream.read(compress);
    Int size;
    stream.read(size);
    if(size < headerSize)
    {
	throw IllegalMessageSizeException(__FILE__, __LINE__);
    }
    if(size > static_cast<Int>(_instance->messageSizeMax()))
    {
	throw MemoryLimitException(__FILE__, __LINE__);
    }
    if(size > static_cast<Int>(stream.b.size()))
    {
	stream.b.resize(size);
    }
    stream.i = stream.b.begin() + pos;
    
    if(stream.i != stream.b.end())
    {
	if(handler->datagram())
	{
	    if(_warnUdp)
	    {
		Warning out(_instance->logger());
		out << "DatagramLimitException: maximum size of " << pos << " exceeded";
		stream.resize(0);
		stream.i = stream.b.begin();
	    }
	    throw DatagramLimitException(__FILE__, __LINE__);
	}
	else
	{
	    handler->read(stream);
	    assert(stream.i == stream.b.end());
	}
    }
}

IceInternal::ThreadPool::EventHandlerThread::EventHandlerThread(const ThreadPoolPtr& pool) :
    _pool(pool)
{
}

void
IceInternal::ThreadPool::EventHandlerThread::run()
{
    bool promote;

    try
    {
	promote = _pool->run();
    }
    catch(const Exception& ex)
    {	
	Error out(_pool->_instance->logger());
	out << "exception in `" << _pool->_prefix << "':\n" << ex; 
	promote = true;
    }
    catch(const std::exception& ex)
    {
	Error out(_pool->_instance->logger());
	out << "std::exception in `" << _pool->_prefix << "':\n" << ex.what();
	promote = true;
    }
    catch(...)
    {
	Error out(_pool->_instance->logger());
	out << "unknown exception in `" << _pool->_prefix << "'"; 
	promote = true;
    }

    if(promote && _pool->_sizeMax > 1)
    {
	//
	// Promote a follower, but w/o modifying _inUse or creating
	// new threads.
	//
	{
	    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*_pool.get());
	    assert(!_pool->_promote);
	    _pool->_promote = true;
	    _pool->notify();
	}
    }

    _pool = 0; // Break cyclic dependency.
}
