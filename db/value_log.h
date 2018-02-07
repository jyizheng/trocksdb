// TODO: if ref added to ring lower than the queued_fileno, update queued_fileno
// consider sorting all keys in an active recycle to reduce number of files referred to after the next compaction

//  Copyright (c) 2017-present, Toshiba Memory America, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#define DEBLEVEL 0x1000  // 1=SST ring ops 2=file ops 4=iterator ops 8=ring pointers 16=deleted_files 32=versions 64=top-level ring ops 128=ring status 256=Versions 512=Audit ref0 1024=Destructors
                        // 0x800=VlogInfo 0x1000=space amp
#define DELAYPROB 0   // percentage of the time a call to ProbDelay will actually delay
#define DELAYTIME std::chrono::milliseconds(10)

#include <list>
#include <forward_list>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include "options/db_options.h"
#include "rocksdb/env.h"
#include "rocksdb/transaction_log.h"
#include "db/version_builder.h"


#if DELAYPROB
#include<chrono>
#include<thread>
#include<stdlib.h>
#endif

#if ATOMIC_POINTER_LOCK_FREE!=2
error: we store pointers without using atomic ops, but that is not thread-safe on this CPU
#endif


namespace rocksdb {
#if DELAYPROB
extern void ProbDelay(void);
#endif

struct FileMetaData;
struct CompactionInputFiles;
class ColumnFamilyData;
class Status;
class Slice;
class VLogRing;
class VersionStorageInfo;

// class that can be used to create a vector of chars that doesn't initialize when you resize
class NoInitChar {
public:
  char data;
  NoInitChar(){};
};

// The Value Log is the place where values are stored separate from their keys.  Values are written to the Value Log
// during compaction.

// There is one VLog per column family, created when the column family is initialized (provided that the
// column family has a table type that supports indirect values).  After options have been vetted and all columns created, the
// Value Log is initialized for operation.

// Each VLog has one or more VLogRings, under programmer control.  Each VLogRing is associated with a set of levels.  Each compaction
// writes to the ring associated with its output level.  The association of output levels to VLogRings can be changed, but it may
// take some time for the values to be migrated to the correct output ring.

// The VLogRing contains the Ring: a list of all the files containing values; and the Queue: for each VLog file, a linked-list of SSTs whose
// earliest reference is to the file.  We use chain fields inside the SST's metadata to hold the chain fields for the Queue.

// Files containing values are sequences of bytes, jammed together with no record marks.  Each value can be compressed and CRCd
// independently.  The filename for a value file is path###.vlg$$$ where
// path is the last db_path for the column family
// ### is an ASCII string of the number of the file
// $$$ is the name of the column family, which must be suitable for inclusion in a file name
//
// With the file number, the ring number occupies the low 2 bits and the upper bits are a sequential number.

// During compaction, each value above a user-specified size is written to a file and replaced by a VLogRingRef, a fixed-length string
// that identifies the ring number, file, offset, and length of the stored value.  Calls to Get() that encounter a VLogRingRef (which
// is identified by a special Value Type) call the VLog to read from disk to replace the indirect reference with the value data.


typedef uint64_t VLogRingRefFileno;
typedef int64_t VLogRingRefFileOffset;   // signed because negative values are used to indicate error
typedef uint64_t VLogRingRefFileLen;

extern void BreakRecordsIntoFiles(std::vector<size_t>& filecumreccnts, std::vector<VLogRingRefFileOffset>& rcdend, int64_t maxfilesize);

// Constants (some will be replaced by options)

static const std::string kRocksDbVLogFileExt = "vlg";   // extension for vlog files is vlgxxx when xxx is CF name
static const VLogRingRefFileno high_value = ((VLogRingRefFileno)-1)>>1;  // biggest positive value
static const float expansion_fraction = 0.25;  // fraction of valid files to leave for expansion
static const int expansion_minimum = 10;  // minimum number of expansion files   // scaf 100
// The deletion deadband is the number of files at the end of the VLog that are protected from deletion.  The problem is that files added to the
// VLog are unreferenced (and unprotected by earlier references) until the Version has started being created.  If the tail pointer gets to such a file while
// it is still unprotected, it will be deleted prematurely.  Keeping track of which files should be released at the end of each compaction is a pain,
// so we simply don't delete files that are within a few compactions of the end.  The deadband is a worst-case estimate of the number of VLog files
// that could be created (in all threads) between the time a compaction starts and the time its Version is ratified
static const int deletion_deadband = 10;  // scaf should be 1000 for multi-VLog files
static const int max_simultaneous_deletions = 1000;  // maximum number of files we can delete in one go.  The limitation is that we have to reserve
   // space for them before we acquire the lock
static const double vlog_remapping_fraction = 0.5;  // References to the oldest VLog files - this fraction of them - will be remapped if encountered during compaction
static const int maxfilesize = 100000;  // scaf largest file to write out

// ParsedFnameRing contains filenumber and ringnumber for a file.  We use it to split the compositie filename/ring into its parts

// Convert file number to file number and ring
class ParsedFnameRing {
public:
  int ringno_;
  uint64_t fileno_;
  ParsedFnameRing(VLogRingRefFileno file_ring) :  // construct from combined fileno/ring
    fileno_(file_ring>>2), ringno_((int)file_ring&3) {}
  ParsedFnameRing(int ring, VLogRingRefFileno file) : ringno_(ring), fileno_(file) {}  // construct from separate ring & file
  int ringno() { return ringno_; }
  VLogRingRefFileno fileno() { return fileno_; }
  VLogRingRefFileno filering() { return fileno_*4 + ringno_; }
};

//
//
// ******************************************** VlogRingRef ***********************************************
// The reference to a VLog file, comprising the ring number, file number, offset, and length.
//
//


// The VLogRingRef is a reference to data in a VLogRing.  It can be converted to an opaque string
// form, whose length is fixed so that references in an SST
// can be easily remapped in place.  The reference indicates the ring, file, offset, and length of the reference
//
// Opaque string holds the 16 bytes from 2 int64s, which are FFFooooo ffrlllll  (file/ring is FFFffr)
class VLogRingRef {
public:
// Constructor
VLogRingRef(int r, VLogRingRefFileno f, VLogRingRefFileOffset o, VLogRingRefFileLen l) : ringno(r), fileno(f), offset(o), len(l) {}  // with data
VLogRingRef() { }   // placeholder only
VLogRingRef(int r) : ringno(r), fileno(0), offset(0), len(0) {}  // ring only
VLogRingRef(int r, VLogRingRefFileno f) : ringno(r), fileno(f), offset(0), len(0) {}  // ring & filenumber only
VLogRingRef(const char *opaqueref) {  // creating ref from OpaqueRef array
  memcpy(workarea.extform,opaqueref,sizeof(workarea.extform));  // move ref to aligned storage
  // extract fields from 64-bit names to be independent of byte order, as long as the machine architecture
  // doesn't change over the life of the database
  offset = workarea.intform[0]&((1LL<<40)-1);
  len = workarea.intform[1]&((1LL<<40)-1);
  ringno = (workarea.intform[1]>>40)&3;
  fileno = ((workarea.intform[0]&(-(1LL<<40)))>>(40-(64-42))) + ((workarea.intform[1]&(-(1LL<<42)))>>(42-0));  // move ff from bit 42 to bit 0; FFF from 40 to 22
}
VLogRingRef(std::string& opaqueref) {  // creating ref from OpaqueRef
  assert(16==opaqueref.size());   // make sure size of ref is right
  VLogRingRef(opaqueref.data());  // make ref from data array
}

// Fill in an existing RingRef
void FillVLogRingRef(int r, VLogRingRefFileno f, VLogRingRefFileOffset o, VLogRingRefFileLen l)
  {ringno=r; fileno=f; offset=o; len=l;}

// Create OpaqueRef (a character array)
void OpaqueRef(char *result) {
  MakeWorkarea();
  memcpy(result,workarea.extform,sizeof(workarea.extform));  // move ref user area
}
void OpaqueRef(std::string &result) {
  MakeWorkarea();
  result.assign(workarea.extform,sizeof(workarea.extform));  // make ref from data array
}

// Create an OpaqueRef in the workarea for this ref and fill in the given slice to point to it
void IndirectSlice(Slice& slice) {
  MakeWorkarea();
  slice.install(workarea.extform,sizeof(workarea.extform));  // point the argument slice to our workarea
}

// Extract portions of the ref
VLogRingRefFileOffset Offset() { return offset; }
VLogRingRefFileLen Len() { return len; }
int Ringno() { return ringno; }
VLogRingRefFileno Fileno() { return fileno; }
// Set portions of the ref
void SetOffset(VLogRingRefFileOffset o) { offset = o; }
void SetFileno(VLogRingRefFileno f) { fileno = f; }
void SetLen(VLogRingRefFileLen l) { len = l; }

// Create Filenumber from RingRef
uint64_t FileNumber() {return (fileno<<2)+ringno;}

static int const sstrefsize {16};  // the size of an indirect reference in the SST

private:
VLogRingRefFileno fileno;
VLogRingRefFileOffset offset;
VLogRingRefFileLen len;
int ringno;
union {
  uint64_t intform[2];   // internal form
  char extform[sstrefsize];   // external form
} workarea;
void MakeWorkarea() {
  assert(offset<=((1LL<<40)-1));  // offset and length should be 5 bytes max
  assert(len<=((1LL<<40)-1));  // offset and length should be 5 bytes max
  assert(fileno<=((1LL<<46)-1));  // fileno should be 46 bits max
  assert(ringno<=3);  // ring should be 2 bits max; fileno+ringno is 48 bits
  workarea.intform[0] = offset + ((((fileno*4)+ringno)&(-(1LL<<(64-40))))<<(40-24));  // isolate FFF starting in bit 24; move to bit 40 of offset
  workarea.intform[1] = len + (((fileno*4)+ringno)<<40);   ///  move ffr to bit 40 of len
}

};

//
//
// ******************************************** VlogRingFile ***********************************************
// Each VLog file is represented by one VLogRingFile entry.  This entty contains the pointer to the opened file, which can
// be used for random reads; a queue of SSTs in the current version whose oldest reference is to this file; and the total
// number of SSTs (including those that are not current but still alive in an earlier version) whose oldest reference
// is to this file.  When the oldest file has no SSTs pointing to it, it can be deleted.
//
//



// A VLogRingFile is a random-access file along with a refcount, queue, and filename.
// It is created to add the file to a VLogRing.  When the file is no longer referenced it is deleted; when
// the VLogRingFile object is deleted, the file is simply closed.
class VLogRingFile {
public:
  // The ring:
  // NOTE that the elements are constructed in the order of their declaration.  In Get() we look at filepointer to see if it is nonnull, so we
  // make it the last to be constructed so that the whole record is valid if the filepointer is.  This is not required by Get() but it's good practice.

  // The queue contains one entry for each entry in the fd_ring.  That entry is a linked list of pointers to the SST files whose earliest
  // reference in this ring is in the corresponding file.  When an SST is created, an element is added, and when the SST is finally deleted, one
  // is removed.
  FileMetaData* queue;  // base of forward chain
  // There are 2 refcounts, one for deciding when to delete the file and one used to decide when to take the file out of the manifest.  Each
  // refcount is incremented when a reference from an SST is created.  The deletion count is decremented only when the last referring SST has been
  // deleted; then it is known that nothing in the current execution refers to the file.  The manifest count is decremented as soon as the last referring
  // SST is marked to be removed from the manifest.  Once that SST is out of the manifest, the VLog file is not needed when the database is reopened.
  // We take the VLog file out of the manifest at the same time we remove its last reference.
  uint32_t refcount_deletion;   // Number of SSTs that hold a reference to this file, including old ones
  uint32_t refcount_manifest;   // Number of current SSTs that hold a reference to this file
  std::unique_ptr<RandomAccessFile> filepointer;  // the open file
  VLogRingRefFileLen length;  // length of the file, needed for fragmentation bookkeeping

  VLogRingFile(std::unique_ptr<RandomAccessFile>& fptr,  // the file 
    VLogRingRefFileLen len   // its length
  ) : queue(nullptr), refcount_deletion(0), refcount_manifest(0), filepointer(std::move(fptr)), length(len) {}
  VLogRingFile() :   // used to initialize empty slot
    queue(nullptr), refcount_deletion(0), refcount_manifest(0), filepointer(nullptr), length(0) {}
  VLogRingFile(VLogRingFile& v, int dummy) :  // used to create faux copy constructor to evade no-copy rules
    queue(v.queue), refcount_deletion(v.refcount_deletion), refcount_manifest(v.refcount_manifest), filepointer(v.filepointer.get()), length(v.length) {}

  // Ensure the file is not copyable
  VLogRingFile(VLogRingFile const&) = delete;
  VLogRingFile& operator=(VLogRingFile const&) = delete;

  // But make it movable, needed for emplace_back
    // Move constructor
  VLogRingFile(VLogRingFile&& other) noexcept
  : queue(other.queue), refcount_deletion(other.refcount_deletion), refcount_manifest(other.refcount_manifest), length(other.length)
  {
    filepointer = std::move(other.filepointer);
    // Now that we have moved, reset the other to empty queue
    other.queue = nullptr;
    other.refcount_deletion = 0;
    other.refcount_manifest = 0;
  }

  // move assignment operator
  VLogRingFile& operator=(VLogRingFile&& rhs) noexcept {
    queue = rhs.queue;
    refcount_deletion = rhs.refcount_deletion;
    refcount_manifest = rhs.refcount_manifest;
    filepointer = std::move(rhs.filepointer);
    length = rhs.length;
    // Now that we have moved, reset the other to empty queue
    rhs.queue = nullptr;
    rhs.refcount_deletion = 0;
    rhs.refcount_manifest = 0;
    return *this;
  }

};

//
//
// ******************************************** VlogRingFileDeletion ***********************************************
// A VLogRingFileDeletion holds just enough information to delete the file.  We transfer ownership of the open file to the Deletion
//
//
class VLogRingFileDeletion {
public:
  VLogRingRefFileno fileno;
  std::unique_ptr<RandomAccessFile> filepointer;  // the open file

  // Constructor
  VLogRingFileDeletion::VLogRingFileDeletion(VLogRingRefFileno fileno_, VLogRingFile& f) : fileno(fileno_), filepointer(std::move(f.filepointer)) {}

  // Delete the file.  Close it as random-access, then delete it
  void DeleteFile(VLogRing& v,   // the current ring
    const ImmutableDBOptions *immdbopts,   // The current Env
    EnvOptions& file_options  // options to use for all VLog files
  );

};

//
//
// ******************************************** VLogRingRestartInfo **************************************
//
// This is the ring information we need on a restart
  struct VLogRingRestartInfo {
    // For stats purposes, and to allow us to schedule Active Recycling effectively, we need to know how much fragmentation there is in the ring.
    // In turn this requires us to keep track of which files have been included in the fragmentation count.  That is, it isn't enough just to
    // look at the files that exist: files may be written while the system is crashing, before references to them have been created, and they will
    // float along until they are deleted, at which time they MUST NOT be treated as removing compaction.  For the oldest files, we need a freed-to mark
    // to indicate which zombie files have been included in the fragmentation count, to ensure we don't count one twice.
    //
    // Normally, the list of files will be a single interval (a,b).  If files get added out-of-order at the end, there may briefly be more intervals.
    // If there is a restart at a moment where there are multiple intervals, those intervals will persist until they are recycled.  The current
    // set of intervals is written to the manifest after each compaction.
    //
    // The intervals are written back-to-back, as start0,end0,start1,end1... each containing a file number for this ring
    //
    // This structure goes into an edit list, which means it is combined with other like structures to produce the current status of the CF.
    std::vector<VLogRingRefFileno> valid_files;  // a sequence of intervals start0,end0,start1,end1... indicating the valid filenumbers
  // statistics are kept for the total size of the ring and the number of bytes of fragmentation.  Fragmentation is the number of bytes in the
  // valid files that are not referred to by any SST.  Fragmentation is added by compaction, which leave gaps in the files.  Fragmentation is removed by
  // deletion of empty files.  The first interval may be 0,end0 which means DELETE all files up to end0
    int64_t size;   // total # bytes in ring
    int64_t frag;   // total # bytes of fragmentation in ring

    // empty constructor.  Could be defaulted
    VLogRingRestartInfo() : valid_files(std::vector<VLogRingRefFileno>()), size(0), frag(0) {}
    // constructor when all parts are known
    VLogRingRestartInfo(std::vector<VLogRingRefFileno> valid_files_, int64_t size_, int64_t frag_) : valid_files(valid_files_), size(size_), frag(frag_) {}

    // Combine two edits into one equivalent composite.  The operation defined here must be left-associative and must work when *this is
    // the empty state.
    void Coalesce(VLogRingRestartInfo& sec, bool outputdeletion);  // fold sec into this

    // see if valid_files contains the given fileno
    bool ContainsFileno(VLogRingRefFileno fno)
      { if(valid_files.size()==0)return false; for(size_t i = valid_files[0]==0?2:0; i<valid_files.size(); i+=2)if(fno>=valid_files[i] && fno<=valid_files[i+1])return true; return false; }
  };

// Routine to coalesce a vector of Ring info.  Must be left-associative, and work when applied to empty arguments.
// Leaves result in 'pri'
extern void Coalesce(std::vector<VLogRingRestartInfo>& pri, std::vector<VLogRingRestartInfo>& sec, bool outputdeletion);


// A VLogRing is a set of sequentially-numbered files, with a common prefix and extension .vlg, that contain
// values for a column family.  Each value is pointed to by an SST, which uses a VLogRingRef for the purpose.
// During compaction, bursts of values are written to the VLogRing, lumped into files of approximately equal size.
//
// Each column family has (possibly many) VLogRing.  VLogRingRef entries in an SST implicitly refer to the VLog of the column family.

//
//
// ******************************************** VlogRing ***********************************************
// VLogRing keeps track of VLogRingFiles for a specified set of levels in the database.  Files are entered into
// the ring during compaction.  The file persists until there are no more SSTs that refer to it.  During compaction,
// the oldest values are picked up & copied to new files, which removes references to the oldest files.
//
// We use the VLogRing to retrieve the value of an indirect reference, given the ring number/file number/offset/length
// of the reference.  We also use the VLogRing to see which SSTs have references to the oldest files.
//



class VLogRing {
friend class VLog;
friend class VLogRingFileDeletion;
friend class IndirectIterator;
friend class RecyclingIterator;
friend void VersionBuilder::SaveTo(VersionStorageInfo*, ColumnFamilyData *);
friend void DetectVLogDeletions(ColumnFamilyData *, std::vector<VLogRingRestartInfo> *);

private:

// We have to cross-index the VLog files and the SSTs for two purposes: (1) to see which VLog files can be deleted when they
// are no longer used by an SST; (2) to see which SSTs are pointing to the oldest VLog files, so we can Active Recycle them or
// give priority to compacting them.
//
// Each SST saves, as part of its metadata, the oldest VLog reference in the SST (there is one of these for each ring in the CF,
// because it is possible though unusual for a single SST to have references to multiple rings).  These VLog references are
// calculated during each compaction and are kept in the Manifest so they can be recovered quickly during recovery.
//
// The SSTs whose earliest reference is to a given VLog file are chained together on a doubly-linked list that is anchored in
// a vector of anchors, one per file.  In addition, each VLog file is assigned a counter indicating how many SSTs hold an earliest-reference
// to it.  An SST is (1) added to the chain when it is created during compaction; (2) removed from the chain
// when it becomes inactive, i. e. no longer in the current view.  Even though the SST is inactive it may be part of a snapshot, so
// the reference counter is not decremented until the SST is finally destroyed (after all snapshots it appears in have been deleted).
//
// When an SST is removed from a ring it is on, its backchain field for that ring is set to popint to the same SST block as a flag
// condition indicating 'not on ring'.  We use that information twhen the SST is deleted: if it has not been removed from its ring,
// we assume that the SST is being deleted as part of a shutdown, and we do not delete the file that it points to.
//
// When the reference count in the oldest VLog file is zero, that file can be deleted.  The tail pointer in the ring indicates the
// earliest file that has not been deleted.
//
// An SST can be chained to more than one ring.  Because the chain anchors may move, end-of-chain is indicated by a nullptr rather
// than a pointer to the root.
//
// The design of the rings is complicated by two considerations: (1) we want to make the Get() path absolutely as fast as possible,
// without any bus-locked read in the normal case; (2) as the database grows, the ring may fill up and have to be relocated.
// In mitigation, reallocation of the ring will be very rare, and any modification, even adding a file, should occur long in advance of the
// first time the file is used as a reference.  We acquire a lock on the ring to make a modification, including reallocation, and we
// double-buffer the ring so that after a reallocation the old ring persists until the next reallocation, which should be days later.

  // The ring:
  std::vector<VLogRingFile> fd_ring[2];  // the rings of open file descriptors for the VLog files.  We ping-pong between two rings

  // We group the atomics together so they can be aligned to a cacheline boundary
  struct /* alignas(64) */ {   // alignment is desirable, but not available till C++17.  This struct is 1 full cache line
  // The ring head/tail pointers:
  // For the initial implementation we write only entire files and thus don't need offsets.
  // The file numbers are actual disk-file numbers.  The conversion function Ringx converts a file number to a ring index.
  std::atomic<uint64_t> currentarrayx;  // the number of the current ring.  The other one is the previous ring.
  std::atomic<VLogRingRefFileno> fd_ring_head_fileno;   // file number of the last file in the ring.
  std::atomic<VLogRingRefFileno> fd_ring_tail_fileno;   // smallest valid file# in ring
  std::atomic<VLogRingRefFileno> fd_ring_queued_fileno;   // The queued fileno is used for speedup,
     // to avoid scanning through empty queues in search of the oldest references.  Formally, the queued points to a file such that all earlier files
     // have empty queues, and no reference to an  earlier file can be generated by any current or future compaction.
  std::atomic<VLogRingRefFileno> fd_ring_prevbuffer_clear_fileno;   // When the headpointer passes this point, we assume that all threads have had
     // enough time to update the ping-pong pointer for the rings, and we clear the previous buffer to save space.
  // The usecount of the current ring.  Set to 0 initially, incr/decr during Get.  Set to a negative value, which
  // quiesces Get, when we need to resize the ring.  We use this as a sync point for the ringpointer/len so we don't have to use atomic reads on them.
  std::atomic<uint32_t> usecount;
  std::atomic<uint32_t> writelock;  // 0 normally, 1 when the ring is in use for writing
  } atomics;

  // All the info we need to preserve over a restart/compaction
  VLogRingRestartInfo restartinfo;

  // Convert a file number to a ring slot in the current ring.  To avoid the divide we require the ring have power-of-2 size
  size_t Ringx(std::vector<VLogRingFile>& fdring, VLogRingRefFileno f) { return (size_t) f & (fdring.size()-1); }

  // Non-ring variables:
  int ringno_;  // The ring number of this ring within its CF
  ColumnFamilyData *cfd_;  // The data for this CF
  const ImmutableDBOptions *immdbopts_;  // Env for this database
  EnvOptions envopts_;  // Options to use for files in this VLog

  // Acquire write lock on the VLogRing.  Won't happen often, and only for a short time
// scaf  should use timed_mutex, provided that generates PAUSE instr
  void AcquireLock() {
    do{
      uint32_t expected_atomic_value = 0;
      if(atomics.writelock.compare_exchange_weak(expected_atomic_value,1,std::memory_order_acq_rel))break;
    }while(1);  // get lock on file
  }

  // Release the lock
  void ReleaseLock() {
  atomics.writelock.store(0,std::memory_order_release);
  }

public:
// Constructor.  Find all the VLogRing files and open them.  Create the VLogRingQueue for the files.
// Delete files that have numbers higher than the last valid one.
VLogRing(
  int ringno,   // ring number of this ring within the CF
  ColumnFamilyData *cfd,  // info for the CF for this ring
  std::vector<std::string> filenames,  // the filenames that might be vlog files for this ring
  std::vector<VLogRingRefFileLen> filesizes,   // corresponding file sizes
  const ImmutableDBOptions *immdbopts,   // The current Env
  EnvOptions& file_options  // options to use for all VLog files
);

  // Ensure the ring is not copyable
  VLogRing(VLogRing const&) = delete;
  VLogRing& operator=(VLogRing const&) = delete;

#if DEBLEVEL&0x400
  ~VLogRing() { printf("Destroying VLogRing %p\n",this); }
#endif

  // See how many files we need to hold the
  // valid files, plus some room for expansion (here, the larger of a fraction of the number of valid files
  // and a constant)
VLogRingRefFileno RingSizeNeeded(VLogRingRefFileno earliest_ref,VLogRingRefFileno latest_ref) {
  VLogRingRefFileno ringexpansion = (int)((latest_ref-earliest_ref+1)*expansion_fraction);
  if(ringexpansion<expansion_minimum)ringexpansion = expansion_minimum;
  // force the ring to a power-of-2 multiple so that Ringx() can be efficient
  ringexpansion = latest_ref-earliest_ref+1+ringexpansion;
  VLogRingRefFileno power2; for(power2 = 1;power2<ringexpansion;power2<<=1);
  return power2;
}

// See how many files need to be deleted.  Result is a vector of them.  We must have the lock
void CollectDeletions(
  VLogRingRefFileno tailfile,  // up-to-date tail file number.  There must be no files before it.  
  VLogRingRefFileno headfile,  // up-to-date head file number
  std::vector<VLogRingFileDeletion>& deleted_files  // result: the files that are ready to be deleted
);

// Delete the files collected by CollectDeletions.  We must have released the lock
void ApplyDeletions(std::vector<VLogRingFileDeletion>& deleted_files) {
  for(size_t i = 0;i<deleted_files.size();++i) {
#if DEBLEVEL&1
  printf("Deleting: %llx\n",deleted_files[i].fileno);
#endif
    deleted_files[i].DeleteFile(*this,immdbopts_,envopts_);
  }
}

// Resize the ring if necessary, updating currentarrayx if it is resized
// If the ring is resized, or if we found that no change was needed, return the new value of currentarrayx
//   (NOTE that in either case tailfile and headfile are NOT modified)
// If someone else took the lock when we laid it down, return 2 to indicate that fact (the resize must be retried)
// Requires holding the ring on entry, and guarantees holding the ring on return, though we might have lost it in between
uint64_t ResizeRingIfNecessary(VLogRingRefFileno tailfile, VLogRingRefFileno headfile, uint64_t currentarray,  // current ring pointers
  size_t numaddedfiles  // number of files we plan to add
);


// Write accumulated bytes to the VLogRing.  First allocate the bytes to files, being
// careful not to split a record, then write them all out.  Create new files as needed, and leave them
// open.  The last file will be open for write; the others are read-only
// The result is a VLogRingRef for the first (of possibly several sequential) file, and a vector indicating the
// number of bytes written to each file
// We housekeep the end-of-VLogRing information
void VLogRingWrite(
std::vector<NoInitChar>& bytes,   // The bytes to be written, jammed together
std::vector<VLogRingRefFileOffset>& rcdend,  // The running length of all records up to and including this one
VLogRingRef& firstdataref,   // result: reference to the first value written
std::vector<VLogRingRefFileOffset>& fileendoffsets,   // result: ending offset of the data written to each file.  The file numbers written are sequential
          // following the one in firstdataref.  The starting offset in the first file is in firstdataref; it is 0 for the others
std::vector<Status>& resultstatus   // place to save error status.  For any file that got an error in writing or reopening,
          // we add the error status to resultstatus and change the sign of the file's entry in fileendoffsets.  (no entry in fileendoffsets
          // can be 0)
)
;

// Read the bytes referred to in the given VLogRingRef.  Uses release-acquire ordering to verify validity of ring
// Returns the bytes.  ?? Should this return to user area to avoid copying?
Status VLogRingGet(
  VLogRingRef& request,  // the values to read
  std::string *response   // the data pointed to by the reference
)
;

// Install a new SST into the ring, with the given earliest-VLog reference
void VLogRingSstInstall(
  FileMetaData& newsst   // the SST that has just been created & filled in
)
;

// Remove an SST from the ring when it is no longer current
void VLogRingSstUnCurrent(
  FileMetaData& retiringsst   // the SST that has just been obsoleted
)
;

// Remove the VLog file's dependency on an SST, and delete the VLog file if it is now unused
void VLogRingSstDelete(
  FileMetaData& expiringsst   // the SST that is about to be destroyed
);

// Return a vector of SSTs that have the smallest oldest-reference-filenumbers.  The maximum number is the capacity of laggingssts; the
// actual number returned will be a number that avoids getting only part of the references to a VLog file, but never less than n unless
// there are fewer SSTs than that.
// We don't return SSTs that are being compacted or are not current.
void VLogRingFindLaggingSsts(
  size_t minfreevlogfiles,  // minimum number of files to free up
  size_t minssts,   // minimum number of SSTs to put into the compaction
  std::vector<CompactionInputFiles>& laggingssts,  // result: vector of SSTs that should be recycled.  The size on entry is the maximum size allowed
  VLogRingRefFileno& lastfile   // result: the last VLog file that is being freed
)
;

};

//
//
// ******************************************** Vlog ***********************************************
// One per CF, containing one or more VLogRings.  Vlog is created for any CF that uses a table type
// that is compatible with indirect values.
//
//

// A VLog is a set of VLogRings for a single column family.  The VLogRings are distinguished by an index.
class VLog {
friend void VersionBuilder::SaveTo(VersionStorageInfo*, ColumnFamilyData *);
friend void DetectVLogDeletions(ColumnFamilyData *, std::vector<VLogRingRestartInfo> *);

private:
  friend class IndirectIterator;
  std::vector<std::unique_ptr<VLogRing>> rings_;  // the VLogRing objects for this CF
  std::vector<int> starting_level_for_ring_;
  ColumnFamilyData *cfd_;
  std::vector<FileMetaData *> waiting_sst_queues;  // queue headers when SSTs are queued awaiting init.  One per possible ring
  std::atomic<uint32_t> writelock;  // 0 normally, 1 when the ring headers are being modified

  // Acquire write lock on the VLog.  Won't happen often, and only for a short time
// scaf  should use timed_mutex, provided that generates PAUSE instr
  void AcquireLock() {
    do{
      uint32_t expected_atomic_value = 0;
      if(writelock.compare_exchange_weak(expected_atomic_value,1,std::memory_order_acq_rel))break;
    }while(1);  // get lock on file
  }

  // Release the lock
  void ReleaseLock() {
  writelock.store(0,std::memory_order_release);
  }

public:
  VLog(
    // the info for the column family
    ColumnFamilyData *cfd
  );
  size_t nrings() { return rings_.size(); }
  std::vector<std::unique_ptr<VLogRing>>& rings() { return rings_; }
  int starting_level_for_ring(int ringno) { return starting_level_for_ring_[ringno]; }

  // No copying
  VLog(VLog const&) = delete;
  VLog& operator=(VLog const&) = delete;

#if DEBLEVEL&0x400
  ~VLog() { printf("Destroying VLog %p\n",this); }
#endif

  // Initialize each ring to make it ready for reading and writing.
  // It would be better to do this when the ring is created, but with the existing interfaces there is
  // no way to get the required information to the constructor
  Status VLogInit(
    std::vector<std::string> vlg_filenames,    // all the filenames that exist for this database - at least, all the vlg files
    std::vector<VLogRingRefFileLen> vlg_filesizes,   // corresponding file sizes
    const ImmutableDBOptions *immdbopts,  // the currect Env
    EnvOptions& file_options   // options to use for VLog files
  )
    // Go through all the SSTs and create a vector of filerefs for each ring

    // For each ring, initialize the ring and queue
;

  // Read the bytes referred to in the given VLogRingRef.  Uses release-acquire ordering to verify validity of ring
  // Returns the bytes.  ?? Should this return to user area to avoid copying?
  Status VLogGet(
    const Slice& reference,  // the reference
    std::string *result   // where the result is built
  )
  // extract the ring# from the reference
  // Call VLogRingGet in the selected ring
;

  // Given the level of an output file, return the ring number, if any, to write to (-1 if no ring)
  int VLogRingNoForLevelOutput(int level) { int i; for(i=0; i<starting_level_for_ring_.size() && level>=starting_level_for_ring_[i];++i); return i-1;}  // advance if output can go into ring; back up to last such

  // Return the VLogRing for the given level
  VLogRing *VLogRingFromNo(int ringno) { return rings_[ringno].get(); }

  // Get info about the file-range that this level feeds into.  We use this to pick compactions
  // in the level we are going to compact.  Results are the ring#, starting value, and range for the ring this level empties into
  // If there are no rings, leave inputs alone.  *file0 has already been set to 0 to indicate that case
  void GetVLogReshapingParms(int level, int *ring, VLogRingRefFileno *file0, VLogRingRefFileno *nfiles) {
    *ring = VLogRingNoForLevelOutput(level+1);  // level for the compaction-result files
    if(*ring>=0) {  // level exists
      // return first file# and range.  We don't have to worry about locking to get a consistent view - approximate values are OK
      VLogRingRefFileno f0 = rings_[*ring]->atomics.fd_ring_queued_fileno;
      VLogRingRefFileno fn = rings_[*ring]->atomics.fd_ring_head_fileno;
      if(fn>f0){  // ring can be empty.  In that case tail is > head and calculations go bad.
        *file0 = f0;  // first file that could have a reference
        *nfiles = fn-f0+1;  // number of files
      }
    }
    return;
  }

  // Install a new SST into the ring, with the given earliest-VLog reference
  // This routine is called whenever a file is added to a column family, which means either
  // during recovery or compaction/ingestion (though note, ingested files have no VLog references and
  // don't need to come through here).  During recovery, the files are encountered before the rings have
  // been created: necessarily, because we don't know how big to make the ring until we have seen what the
  // earliest reference is.  So, if we get called before the rings have been created, we chain them onto
  // a waiting list (one list per eventual ring, using the same chain fields that will normally be used for
  // the doubly-linked list of SSTs per VLog file) and then process them en bloc when the rings are created.
  // Whether this is good design or a contemptible kludge is a matter of opinion.  To be sure, it would
  // be possible to avoid queueing the SSTs by simply using the list of SSTs in each CF as the files to add to the rings
  //
  // We detect pre-initialization by the absence of rings (i. e. rings_.size()==0).  If the CF doesn't turn on
  // indirect values, the number of rings will simply stay at 0.  We will make sure to take no action in removing from the
  // rings in that case.
  //
  // It is OVERWHELMINGLY likely that there will be exactly one nonzero earliest-ref.  The only way to have more
  // is for the ring boundaries to change during operation, and the only way to have less is for an SST to have no
  // indirect values (either too short or all deletes or the like).  Nevertheless we lock most operations at the VLogRing level
  // rather than the VLog level, because it is possible to have compactions going on to the same CF at different levels.
  // In any case all locks are short-lived.
  //
  // We could avoid locks on the chain operations if we were sure that all SST operations held the SST mutex.  They probably do,
  // but I can't guarantee it.  We would still need locks on the queue operations because VLogRingWrite does NOT require the mutex
  // and has to interlock with Get() without locking.
  void VLogSstInstall(
    FileMetaData& newsst   // the SST that has just been created & filled in
  );
  // Remove an SST from the ring when it is no longer current.
  void VLogSstUnCurrent(
    FileMetaData& retiringsst   // the SST that has just been obsoleted
  );

  // Remove the VLog file's dependency on an SST, and delete the VLog file if it is now unused
  void VLogSstDelete(
    FileMetaData& expiringsst   // the SST that is about to be destroyed
  );

};


} // namespace rocksdb
	
