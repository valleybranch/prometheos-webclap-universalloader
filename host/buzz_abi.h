#pragma once
#include <stdint.h>

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned long dword;

enum CMPType { pt_note = 0, pt_switch = 1, pt_byte = 2, pt_word = 3 };

enum {
  MT_MASTER = 0,
  MT_GENERATOR = 1,
  MT_EFFECT = 2
};

enum {
  WM_NOIO = 0,
  WM_READ = 1,
  WM_WRITE = 2,
  WM_READWRITE = 3
};

struct CMachineParameter {
  CMPType Type;
  char const *Name;
  char const *Description;
  int MinValue;
  int MaxValue;
  int NoValue;
  int Flags;
  int DefValue;
};

struct CMachineAttribute {
  char const *Name;
  int MinValue;
  int MaxValue;
  int DefValue;
};

struct CMachineInfo {
  int Type;
  int Version;
  int Flags;
  int minTracks;
  int maxTracks;
  int numGlobalParameters;
  int numTrackParameters;
  CMachineParameter const **Parameters;
  int numAttributes;
  CMachineAttribute const **Attributes;
  char const *Name;
  char const *ShortName;
  char const *Author;
  char const *Commands;
};

struct CMasterInfo {
  int BeatsPerMin;
  int TicksPerBeat;
  int SamplesPerSec;
  int SamplesPerTick;
  int PosInTick;
  float TicksPerSec;
};

class CMachineDataInput {
public:
  virtual void Read(void *pbuf, int const numbytes) = 0;
};

class CMachineDataOutput {
public:
  virtual void Write(void *pbuf, int const numbytes) = 0;
};

class CMICallbacks;

class CMachineInterface {
public:
  virtual ~CMachineInterface() {}
  virtual void Init(CMachineDataInput * const pi) = 0;
  virtual void Tick() = 0;
  virtual bool Work(float *psamples, int numsamples, int const mode) = 0;
  virtual void Stop() = 0;
  virtual void Save(CMachineDataOutput * const po) {}
  virtual void AttributesChanged() {}
  virtual void Command(int const) {}
  virtual void SetNumTracks(int const n) {}
  virtual void MuteTrack(int const) {}
  virtual bool IsTrackMuted(int const) { return false; }
  virtual void MidiNote(int const, int const, int const) {}
  virtual void Event(dword const) {}
  virtual char const *DescribeValue(int const, int const) { return nullptr; }

  void *GlobalVals = nullptr;
  void *TrackVals = nullptr;
  int *AttrVals = nullptr;
  CMasterInfo *pMasterInfo = nullptr;
  CMICallbacks *pCB = nullptr;
};

/* The historical MI15 callback vtable is intentionally conservative here.
 * The implementation supplies the commonly used headless services and leaves
 * unsupported song/UI services as no-ops. */
class CMICallbacks {
public:
  virtual ~CMICallbacks() {}
  virtual void *GetWave(int const) { return nullptr; }
  virtual void *GetWaveLevel(int const, int const) { return nullptr; }
  virtual void MessageBox(char const *) {}
  virtual void Lock() {}
  virtual void Unlock() {}
  virtual int GetWritePos() { return 0; }
  virtual int GetPlayPos() { return 0; }
  virtual float *GetAuxBuffer() { return nullptr; }
  virtual void ClearAuxBuffer() {}
  virtual int GetFreeWave() { return 0; }
  virtual bool AllocateWave(int const, int const, char const *) { return false; }
  virtual void ScheduleEvent(int const, dword const) {}
  virtual void MidiOut(int const, dword const) {}
  virtual short const *GetOscillatorTable(int const) { return nullptr; }
};
