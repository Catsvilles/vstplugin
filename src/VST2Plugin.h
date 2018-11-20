// VST2Plugin

#include "VSTPluginInterface.h"

//#include "aeffect.h"
#include "aeffectx.h"
// #include "vstfxstore.h"

// Plugin's entry point
typedef AEffect *(*vstPluginFuncPtr)(audioMasterCallback);

// AEffectDispatcherProc
// AEffectProcessProc
// AEffectSetParameterProc
// AEffectGetParameterProc

class VST2Plugin final : public IVSTPlugin {
 public:
    static VstIntPtr VSTCALLBACK hostCallback(AEffect *plugin, VstInt32 opcode,
        VstInt32 index, VstIntPtr value, void *ptr, float opt);

    VST2Plugin(void* plugin, const std::string& path);
    ~VST2Plugin();

    std::string getPluginName() const override;
    int getPluginVersion() const override;
    int getPluginUniqueID() const override;

    void process(float **inputs, float **outputs, int nsamples) override;
    void processDouble(double **inputs, double **outputs, int nsamples) override;
    bool hasSinglePrecision() const override;
    bool hasDoublePrecision() const override;
    void suspend() override;
    void resume() override;
    void setSampleRate(float sr) override;
    void setBlockSize(int n) override;
    int getNumInputs() const override;
    int getNumOutputs() const override;
    bool isSynth() const override;
    bool hasTail() const override;
    int getTailSize() const override;
    bool hasBypass() const override;
    void setBypass(bool bypass) override;

    void setParameter(int index, float value) override;
    float getParameter(int index) const override;
    std::string getParameterName(int index) const override;
    std::string getParameterLabel(int index) const override;
    std::string getParameterDisplay(int index) const override;
    int getNumParameters() const override;

    void setProgram(int program) override;
    void setProgramName(const std::string& name) override;
    int getProgram() const override;
    std::string getProgramName() const override;
    std::string getProgramNameIndexed(int index) const override;
    int getNumPrograms() const override;

    bool hasChunkData() const override;
    void setProgramChunkData(const void *data, size_t size) override;
    void getProgramChunkData(void **data, size_t *size) const override;
    void setBankChunkData(const void *data, size_t size) override;
    void getBankChunkData(void **data, size_t *size) const override;

    bool readProgramFile(const std::string& path) override;
    bool readProgramData(const char *data, size_t size) override;
    bool readProgramData(const std::string& buffer) override {
        return readProgramData(buffer.data(), buffer.size());
    }
    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    bool readBankFile(const std::string& path) override;
    bool readBankData(const char *data, size_t size) override;
    bool readBankData(const std::string& buffer) override {
        return readBankData(buffer.data(), buffer.size());
    }
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    bool hasEditor() const override;
    void openEditor(void *window) override;
    void closeEditor() override;
    void getEditorRect(int &left, int &top, int &right, int &bottom) const override;
 private:
    std::string getBaseName() const;
    bool hasFlag(VstAEffectFlags flag) const;
    bool canDo(const char *what) const;
    VstIntPtr dispatch(VstInt32 opCode, VstInt32 index = 0, VstIntPtr value = 0,
        void *ptr = 0, float opt = 0) const;
        // data members
    AEffect *plugin_ = nullptr;
    std::string path_;
};
