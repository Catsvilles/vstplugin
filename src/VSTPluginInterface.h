#pragma once

#include <string>

class IVSTWindow {
 public:
    virtual ~IVSTWindow() {}

    virtual void* getHandle() = 0; // get system-specific handle to the window
	virtual void run() = 0; // run a message loop for this window
	// virtual void quit() = 0; // post quit message

    virtual void setTitle(const std::string& title) = 0;
    virtual void setGeometry(int left, int top, int right, int bottom) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void minimize() = 0;
    virtual void restore() = 0; // un-minimize
    virtual void bringToTop() = 0;
};

// creates a platform dependend window
namespace VSTWindowFactory {
		// call this once before you create any windows. not thread safe (yet)
    void initialize();
		// make a new window. on some platforms you need to pass a context (e.g. Display* on X11)
    IVSTWindow* create(void *context = nullptr);
}

struct VSTMidiEvent {
    VSTMidiEvent(char status = 0, char data1 = 0, char data2 = 0, int _delta = 0){
        data[0] = status; data[1] = data1; data[2] = data2; delta = _delta;
    }
    char data[3];
    int delta;
};

struct VSTSysexEvent {
    VSTSysexEvent(const char *_data, size_t _size, int _delta = 0)
        : data(_data, _size), delta(_delta){}
    template <typename T>
    VSTSysexEvent(T&& _data, int _delta = 0)
        : data(std::forward<T>(_data)), delta(_delta){}
    VSTSysexEvent() = default;
    std::string data;
    int delta;
};

class IVSTPluginListener {
 public:
    virtual ~IVSTPluginListener(){}
    virtual void parameterAutomated(int index, float value) = 0;
    virtual void midiEvent(const VSTMidiEvent& event) = 0;
    virtual void sysexEvent(const VSTSysexEvent& event) = 0;
};

enum class VSTProcessPrecision {
    Single,
    Double
};

class IVSTPlugin {
 public:
    virtual ~IVSTPlugin(){}
    virtual std::string getPluginName() const = 0;
    virtual int getPluginVersion() const = 0;
    virtual int getPluginUniqueID() const = 0;

    virtual void process(float **inputs, float **outputs, int nsamples) = 0;
    virtual void processDouble(double **inputs, double **outputs, int nsamples) = 0;
    virtual bool hasPrecision(VSTProcessPrecision precision) const = 0;
    virtual void setPrecision(VSTProcessPrecision precision) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void setSampleRate(float sr) = 0;
    virtual void setBlockSize(int n) = 0;
    virtual int getNumInputs() const = 0;
    virtual int getNumOutputs() const = 0;
    virtual bool isSynth() const = 0;
    virtual bool hasTail() const = 0;
    virtual int getTailSize() const = 0;
    virtual bool hasBypass() const = 0;
    virtual void setBypass(bool bypass) = 0;

    virtual void setListener(IVSTPluginListener *listener) = 0;

    virtual void setTempoBPM(double tempo) = 0;
    virtual void setTimeSignature(int numerator, int denominator) = 0;
    virtual void setTransportPlaying(bool play) = 0;
    virtual void setTransportRecording(bool record) = 0;
    virtual void setTransportAutomationWriting(bool writing) = 0;
    virtual void setTransportAutomationReading(bool reading) = 0;
    virtual void setTransportCycleActive(bool active) = 0;
    virtual void setTransportCycleStart(double beat) = 0;
    virtual void setTransportCycleEnd(double beat) = 0;
    virtual void setTransportBarStartPosition(double beat) = 0;
    virtual void setTransportPosition(double beat) = 0;
    virtual double getTransportPosition() const = 0;

    virtual int getNumMidiInputChannels() const = 0;
    virtual int getNumMidiOutputChannels() const = 0;
    virtual bool hasMidiInput() const = 0;
    virtual bool hasMidiOutput() const = 0;
    virtual void sendMidiEvent(const VSTMidiEvent& event) = 0;
    virtual void sendSysexEvent(const VSTSysexEvent& event) = 0;

    virtual void setParameter(int index, float value) = 0;
    virtual bool setParameter(int index, const std::string& str) = 0;
    virtual float getParameter(int index) const = 0;
    virtual std::string getParameterName(int index) const = 0;
    virtual std::string getParameterLabel(int index) const = 0;
    virtual std::string getParameterDisplay(int index) const = 0;
    virtual int getNumParameters() const = 0;

    virtual void setProgram(int index) = 0;
    virtual void setProgramName(const std::string& name) = 0;
    virtual int getProgram() const = 0;
    virtual std::string getProgramName() const = 0;
    virtual std::string getProgramNameIndexed(int index) const = 0;
    virtual int getNumPrograms() const = 0;

    virtual bool hasChunkData() const = 0;
    virtual void setProgramChunkData(const void *data, size_t size) = 0;
    virtual void getProgramChunkData(void **data, size_t *size) const = 0;
    virtual void setBankChunkData(const void *data, size_t size) = 0;
    virtual void getBankChunkData(void **data, size_t *size) const = 0;

    virtual bool readProgramFile(const std::string& path) = 0;
    virtual bool readProgramData(const char *data, size_t size) = 0;
    virtual bool readProgramData(const std::string& buffer) = 0;
    virtual void writeProgramFile(const std::string& path) = 0;
    virtual void writeProgramData(std::string& buffer) = 0;
    virtual bool readBankFile(const std::string& path) = 0;
    virtual bool readBankData(const char *data, size_t size) = 0;
    virtual bool readBankData(const std::string& buffer) = 0;
    virtual void writeBankFile(const std::string& path) = 0;
    virtual void writeBankData(std::string& buffer) = 0;

    virtual bool hasEditor() const = 0;
    virtual void openEditor(void *window) = 0;
    virtual void closeEditor() = 0;
    virtual void getEditorRect(int &left, int &top, int &right, int &bottom) const = 0;
};

// expects a path to the actual plugin file (e.g. "myplugin.dll" on Windows,
// "myplugin.so" on Linux, "myplugin.vst" on Apple).
// use 'makeVSTPluginFilePath' to avoid typing the extension
IVSTPlugin* loadVSTPlugin(const std::string& path);

void freeVSTPlugin(IVSTPlugin* plugin);

// check the path and append platform specific extension (if needed)
std::string makeVSTPluginFilePath(const std::string& path);
