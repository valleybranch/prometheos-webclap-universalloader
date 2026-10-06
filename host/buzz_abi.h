#pragma once
#include <stdint.h>

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned long dword;

enum CMPType { pt_note = 0, pt_switch = 1, pt_byte = 2, pt_word = 3 };
enum { MT_MASTER = 0, MT_GENERATOR = 1, MT_EFFECT = 2 };
enum { WM_NOIO = 0, WM_READ = 1, WM_WRITE = 2, WM_READWRITE = 3 };
enum { SF_PLAYING = 1, SF_RECORDING = 2 };

struct CMachineParameter {
  CMPType Type; char const *Name; char const *Description;
  int MinValue, MaxValue, NoValue, Flags, DefValue;
};
struct CMachineAttribute { char const *Name; int MinValue, MaxValue, DefValue; };
struct CMasterInfo {
  int BeatsPerMin, TicksPerBeat, SamplesPerSec, SamplesPerTick, PosInTick;
  float TicksPerSec;
};
struct CWaveInfo { int Flags; float Volume; };
struct CWaveLevel {
  int numSamples; short *pSamples; int RootNote, SamplesPerSec, LoopStart, LoopEnd;
};
struct CEnvelopeInfo { char const *Name; int Flags; };
class CPattern; class CSequence; class CMachine; class CMachineInterfaceEx;
class CMachineDataOutput; class CMachineInfo; class CMachineInterface;
typedef bool (CMachineInterface::*EVENT_HANDLER_PTR)(void *);

class CMachineDataInput { public: virtual void Read(void *, int const) {} };
class CMachineDataOutput { public: virtual void Write(void *, int const) {} };

class CMICallbacks {
public:
  /* MI15 vtable order. No virtual destructor: adding one changes every slot. */
  virtual CWaveInfo const *GetWave(int const) { return nullptr; }
  virtual CWaveLevel const *GetWaveLevel(int const, int const) { return nullptr; }
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
  virtual int GetEnvSize(int const, int const) { return 0; }
  virtual bool GetEnvPoint(int const, int const, int const, word &, word &, int &) { return false; }
  virtual CWaveLevel const *GetNearestWaveLevel(int const, int const) { return nullptr; }
  virtual void SetNumberOfTracks(int const) {}
  virtual CPattern *CreatePattern(char const *, int const) { return nullptr; }
  virtual CPattern *GetPattern(int const) { return nullptr; }
  virtual char const *GetPatternName(CPattern *) { return nullptr; }
  virtual void RenamePattern(char const *, char const *) {}
  virtual void DeletePattern(CPattern *) {}
  virtual int GetPatternData(CPattern *, int const, int const, int const, int const) { return 0; }
  virtual void SetPatternData(CPattern *, int const, int const, int const, int const, int const) {}
  virtual CSequence *CreateSequence() { return nullptr; }
  virtual void DeleteSequence(CSequence *) {}
  virtual CPattern *GetSequenceData(int const) { return nullptr; }
  virtual void SetSequenceData(int const, CPattern *) {}
  virtual void SetMachineInterfaceEx(CMachineInterfaceEx *) {}
  virtual void ControlChange__obsolete__(int, int, int, int) {}
  virtual int ADGetnumChannels(bool) { return 0; }
  virtual void ADWrite(int, float *, int) {}
  virtual void ADRead(int, float *, int) {}
  virtual CMachine *GetThisMachine() { return nullptr; }
  virtual void ControlChange(CMachine *, int, int, int, int) {}
  virtual CSequence *GetPlayingSequence(CMachine *) { return nullptr; }
  virtual void *GetPlayingRow(CSequence *, int, int) { return nullptr; }
  virtual int GetStateFlags() { return 0; }
  virtual void SetnumOutputChannels(CMachine *, int) {}
  virtual void SetEventHandler(CMachine *, int, EVENT_HANDLER_PTR, void *) {}
  virtual char const *GetWaveName(int const) { return nullptr; }
  virtual void SetInternalWaveName(CMachine *, int const, char const *) {}
  virtual void GetMachineNames(CMachineDataOutput *) {}
  virtual CMachine *GetMachine(char const *) { return nullptr; }
  virtual CMachineInfo const *GetMachineInfo(CMachine *) { return nullptr; }
  virtual char const *GetMachineName(CMachine *) { return nullptr; }
  virtual bool GetInput(int, float *, int, bool, float *) { return false; }
};

class CMachineInfo {
public:
  int Type, Version, Flags, minTracks, maxTracks, numGlobalParameters, numTrackParameters;
  CMachineParameter const **Parameters;
  int numAttributes; CMachineAttribute const **Attributes;
  char const *Name, *ShortName, *Author, *Commands;
  void *pLI;
};

struct CMachineInterface {
  /* MSVC x86 object layout. Virtual calls are made by explicit slot number in
   * buzz_instance.cpp; MinGW must not impose its own destructor/vtable ABI. */
  void **vtable;
  void *GlobalVals;
  void *TrackVals;
  int *AttrVals;
  CMasterInfo *pMasterInfo;
  CMICallbacks *pCB;
};
