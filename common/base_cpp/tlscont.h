/****************************************************************************
 * Copyright (C) 2009-2013 GGA Software Services LLC
 * 
 * This file is part of Indigo toolkit.
 * 
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 3 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 * 
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 ***************************************************************************/

#ifndef __tlscont_h__
#define __tlscont_h__

#include "base_c/defs.h"
#include "base_cpp/array.h"
#include "base_cpp/pool.h"
#include "base_cpp/os_sync_wrapper.h"
#include "base_cpp/red_black.h"
#include "base_cpp/ptr_array.h"
#include "base_c/os_tls.h"
#include "base_cpp/auto_ptr.h"

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable:4251)
#endif

namespace indigo {

// Session identifiers manager.
// Every thread have local session ID that corresponds to the all
// local session variables.
class DLLEXPORT _SIDManager {
public:
   static _SIDManager& getInst (void);
   ~_SIDManager (void);

   void  setSessionId     (qword id);
   qword allocSessionId   (void);
   qword getSessionId     (void);
   // Add specified SID to the vacant list. 
   // This method should be called before thread exit if SID was 
   // assigned automatically (not by manual TL_SET_SESSION_ID call)
   void releaseSessionId (qword id);

   DECL_ERROR;

private:
   _SIDManager (void);
   qword * _getID         () const;

   // Thread local key for storing current session ID
   TLS_IDX_TYPE _tlsIdx;
   RedBlackSet<qword> _allSIDs;
   qword _lastNewSID;
   // Array with vacant SIDs
   Array<qword> _vacantSIDs;

   static _SIDManager _instance;   
   static OsLock _lock;
};

// Macros for managing session IDs for current thread
#define TL_GET_SESSION_ID()       _SIDManager::getInst().getSessionId()
#define TL_SET_SESSION_ID(id)     _SIDManager::getInst().setSessionId(id)
#define TL_ALLOC_SESSION_ID()     _SIDManager::getInst().allocSessionId()
#define TL_RELEASE_SESSION_ID(id) _SIDManager::getInst().releaseSessionId(id)

// Container that keeps one instance of specifed type per session
template <typename T>
class _SessionLocalContainer {
public:
   T& getLocalCopy (void)
   {
      return getLocalCopy(_SIDManager::getInst().getSessionId());
   }

   T& getLocalCopy (const qword id)
   {
      OsLocker locker(_lock.ref());
      AutoPtr<T>& ptr = _map.findOrInsert(id);
      if (ptr.get() == NULL)
         ptr.reset(new T());
      return ptr.ref();
   }

private:
   typedef RedBlackObjMap<qword, AutoPtr<T> > _Map;

   _Map           _map;
   ThreadSafeStaticObj<OsLock> _lock;
};

// Macros for working with global variables per each session
// By tradition this macros start with TL_, but should start with SL_
#define TL_DECL_EXT(type, name) extern _SessionLocalContainer< type > TLSCONT_##name
#define TL_DECL(type, name) static _SessionLocalContainer< type > TLSCONT_##name
#define TL_GET(type, name) type& name = (TLSCONT_##name).getLocalCopy()
#define TL_DECL_GET(type, name) TL_DECL(type, name); TL_GET(type, name)
#define TL_GET2(type, name, realname) type& name = (TLSCONT_##realname).getLocalCopy()
#define TL_GET_BY_ID(type, name, id) type& name = (TLSCONT_##name).getLocalCopy(id)
#define TL_DEF(className, type, name) _SessionLocalContainer< type > className::TLSCONT_##name
#define TL_DEF_EXT(type, name) _SessionLocalContainer< type > TLSCONT_##name

// Pool for local variables, reused in consecutive function calls, 
// but not required to preserve their state
template <typename T>
class _ReusableVariablesPool {
public:
   _ReusableVariablesPool  () { is_valid = true; }
   ~_ReusableVariablesPool () { is_valid = false; }
   bool isValid () const { return is_valid; }

   T& getVacant (int& idx)
   {  
      OsLocker locker(_lock);
      if (_vacant_indices.size() != 0)
      {
         idx = _vacant_indices.pop();
         return *_objects[idx];
      }
      _objects.add(new T);
      idx = _objects.size() - 1;
      _vacant_indices.reserve(idx + 1);
      return *_objects[idx];
   }

   void release (int idx)
   {
      OsLocker locker(_lock);
      _vacant_indices.push(idx);
   }

   T& getByIndex (int idx)
   {
      return *_objects[idx];
   }

private:
   OsLock _lock;
   bool is_valid;

   PtrArray< T > _objects;
   Array<int> _vacant_indices;
};

// Utility class for automatically release call
template <typename T>
class _ReusableVariablesAutoRelease {
public:
   _ReusableVariablesAutoRelease () : _idx(-1), _var_pool(0) {}
   
   void init (int idx, _ReusableVariablesPool< T > *var_pool) 
   {
      _idx = idx;
      _var_pool = var_pool;
   }

   ~_ReusableVariablesAutoRelease (void)
   {
      if (_var_pool == 0)
         return;
      // Check if the _var_pool destructor have not been called already
      // (this can happen on program exit)
      if (_var_pool->isValid())
         _var_pool->release(_idx);
   }
protected:
   int _idx;
   _ReusableVariablesPool< T >* _var_pool;
};

// Abstract proxy class to call a destructor for an allocated data
class Destructor
{
public:
   virtual void callDestructor (void *data) = 0;

   virtual ~Destructor() {};
};

// Proxy destructor class for a type T
template <typename T>
class DestructorT : public Destructor
{
public:
   virtual void callDestructor (void *data)
   {
      ((T*)data)->~T();
   }
};

// Template function to create proxy destructor
template <typename T>
Destructor *createDestructor (T *t)
{
   return new DestructorT<T>();
}

// Variables pool that can reuse objects allocations that are initialized in the same order
class _LocalVariablesPool
{
public:
   _LocalVariablesPool ()
   {
      reset();
   }

   ~_LocalVariablesPool ()
   {
      for (int i = 0; i < data.size(); i++)
      {
         destructors[i]->callDestructor(data[i]);
         free(data[i]);
      }
   }

   template <typename T>
   size_t hash ()
   {
      // Use simple and fast class size as a class hash to check that initialization order is the same
      return sizeof(T);
   }

   template <typename T>
   T& getVacant ()
   {
      data.expandFill(index + 1, 0);
      destructors.expand(index + 1);
      type_hash.expandFill(index + 1, 0);

      if (data[index] == 0)
      {
         // Allocate data and destructor
         data[index] = malloc(sizeof(T));
         T *t = new (data[index]) T();
         destructors[index] = createDestructor(t);

         type_hash[index] = hash<T>();
      }
         
      // Class hash check to verify initialization order
      if (type_hash[index] != hash<T>())
         throw Exception("VariablesPool: invalid initialization order");

      T *t = (T*)data[index];
      index++;
      return *t;
   }

   void reset ()
   {
      index = 0;
   }

private:
   Array<void *> data;
   Array<size_t> type_hash;
   PtrArray<Destructor> destructors;
   int index;
};

// Auto release class that additionally calls reset method for LocalVariablesPool 
class _LocalVariablesPoolAutoRelease : public _ReusableVariablesAutoRelease<_LocalVariablesPool>
{
public:
   ~_LocalVariablesPoolAutoRelease ()
   {
      if (_var_pool == 0)
         return;
      if (_var_pool->isValid())
      {
         _LocalVariablesPool &local_pool = _var_pool->getByIndex(_idx);
         local_pool.reset();
      }
   }
};                   


}

// "Quasi-static" variable definition
#define QS_DEF(TYPE, name) \
   static ThreadSafeStaticObj<_ReusableVariablesPool< TYPE > > _POOL_##name; \
   int _POOL_##name##_idx;                                             \
   TYPE &name = _POOL_##name->getVacant(_POOL_##name##_idx);           \
   _ReusableVariablesAutoRelease< TYPE > _POOL_##name##_auto_release;  \
   _POOL_##name##_auto_release.init(_POOL_##name##_idx, _POOL_##name.ptr())

//
// Reusable class members definition
// By tradition this macros start with TL_, but should start with SL_
// To work with them you should first define commom pool with CP_DECL,
// then define it in the source class with CP_DEF(cls), and initialize
// in the constructor via CP_INIT before any TL_CP_ initializations
//

// Add this to class definition  
#define TL_CP_DECL(TYPE, name) \
   typedef TYPE _##name##_TYPE; \
   TYPE &name

// Add this to constructor initialization list
#define TL_CP_GET(name) \
   name(_local_pool.getVacant<_##name##_TYPE>())

#define CP_DECL \
   _LocalVariablesPoolAutoRelease _local_pool_autorelease;                                      \
   static _LocalVariablesPool& _getLocalPool (_LocalVariablesPoolAutoRelease &auto_release);    \
   _LocalVariablesPool &_local_pool                                                             \

#define CP_INIT _local_pool(_getLocalPool(_local_pool_autorelease))

#define CP_DEF(cls) \
   _LocalVariablesPool& cls::_getLocalPool (_LocalVariablesPoolAutoRelease &auto_release)       \
   {                                                                                            \
      static ThreadSafeStaticObj< _ReusableVariablesPool< _LocalVariablesPool > > _shared_pool; \
                                                                                                \
      int idx;                                                                                  \
      _LocalVariablesPool &var = _shared_pool->getVacant(idx);                                  \
      auto_release.init(idx, _shared_pool.ptr());                                               \
      return var;                                                                               \
   }                                                                                            \

#ifdef _WIN32
#pragma warning(pop)
#endif

#endif // __tlscont_h__
