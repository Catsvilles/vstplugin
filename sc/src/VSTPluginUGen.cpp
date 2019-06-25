#include "VSTPluginUGen.h"

#ifdef SUPERNOVA
#include <nova-tt/spin_lock.hpp>
#endif

static InterfaceTable *ft;

void SCLog(const std::string& msg){
    Print(msg.c_str());
}

// SndBuffers
static void syncBuffer(World *world, int32 index) {
    auto src = world->mSndBufsNonRealTimeMirror + index;
    auto dest = world->mSndBufs + index;
    dest->samplerate = src->samplerate;
    dest->sampledur = src->sampledur;
    dest->data = src->data;
    dest->channels = src->channels;
    dest->samples = src->samples;
    dest->frames = src->frames;
    dest->mask = src->mask;
    dest->mask1 = src->mask1;
    dest->coord = src->coord;
    dest->sndfile = src->sndfile;
#ifdef SUPERNOVA
    dest->isLocal = src->isLocal;
#endif
    world->mSndBufUpdates[index].writes++;
}

static void allocReadBuffer(SndBuf* buf, const std::string& data) {
    auto n = data.size();
    BufAlloc(buf, 1, n, 1.0);
    for (int i = 0; i < n; ++i) {
        buf->data[i] = data[i];
    }
}

static void writeBuffer(SndBuf* buf, std::string& data) {
    auto n = buf->frames;
    data.resize(n);
    for (int i = 0; i < n; ++i) {
        data[i] = buf->data[i];
    }
}

// InfoCmdData
InfoCmdData* InfoCmdData::create(World* world, int size) {
    auto data = RTAlloc(world, sizeof(InfoCmdData) + size);
    if (data) {
        new (data)InfoCmdData();
    }
    else {
        LOG_ERROR("RTAlloc failed!");
    }
    return (InfoCmdData *)data;
}

bool InfoCmdData::nrtFree(World* inWorld, void* cmdData) {
    auto data = (InfoCmdData*)cmdData;
    // this is potentially dangerous because NRTFree internally uses free()
    // while BufFreeCmd::Stage4() uses free_aligned().
    // On the other hand, the client is supposed to pass a unused bufnum
    // so we don't have to free any previous data.
    // The SndBuf is then freed by the client.
    if (data->freeData)
        NRTFree(data->freeData);
    return true;
}

// format: size, chars...
int string2floatArray(const std::string& src, float *dest, int maxSize) {
    int len = std::min<int>(src.size(), maxSize-1);
    if (len >= 0) {
        *dest++ = len;
        for (int i = 0; i < len; ++i) {
            dest[i] = src[i];
        }
        return len + 1;
    }
    else {
        return 0;
    }
}

// search and probe
static bool gSearching = false;

static VSTPluginManager gPluginManager;

// VST2: plug-in name
// VST3: plug-in name + ".vst3"
static std::string makeKey(const VSTPluginDesc& desc) {
    std::string key;
    auto ext = ".vst3";
    auto onset = std::max<size_t>(0, desc.path.size() - strlen(ext));
    if (desc.path.find(ext, onset) != std::string::npos) {
        key = desc.name + ext;
    }
    else {
        key = desc.name;
    }
    return key;
}

static void addPlugins(const IVSTFactory& factory) {
    auto plugins = factory.plugins();
    for (auto& plugin : plugins) {
        if (plugin->valid()) {
            gPluginManager.addPlugin(makeKey(*plugin), plugin);
            // LOG_DEBUG("added plugin " << plugin->name);
        }
    }
}

static IVSTFactory* probePlugin(const std::string& path, bool verbose) {
    if (gPluginManager.findFactory(path)) {
        LOG_WARNING("probePlugin: '" << path << "' already probed!");
        return nullptr;
    }
    // load factory and probe plugins
    if (verbose) Print("probing %s... ", path.c_str());
    auto factory = IVSTFactory::load(path);
    if (!factory) {
#if 1
        if (verbose) Print("failed!\n");
#endif
        return nullptr;
    }
    factory->probe();
    auto plugins = factory->plugins();

    auto postResult = [](ProbeResult pr) {
        switch (pr) {
        case ProbeResult::success:
            Print("ok!\n");
            break;
        case ProbeResult::fail:
            Print("failed!\n");
            break;
        case ProbeResult::crash:
            Print("crashed!\n");
            break;
        case ProbeResult::error:
            Print("error!\n");
            break;
        default:
            Print("bug: probePlugin\n");
            break;
        }
    };

    if (plugins.size() == 1) {
        auto& plugin = plugins[0];
        if (verbose) postResult(plugin->probeResult);
        // factories with a single plugin can also be aliased by their file path(s)
        gPluginManager.addPlugin(plugin->path, plugin);
        gPluginManager.addPlugin(path, plugin);
    }
    else {
        Print("\n");
        for (auto& plugin : plugins) {
            if (!plugin->name.empty()) {
                if (verbose) Print("  '%s' ", plugin->name.c_str());
            }
            else {
                if (verbose) Print("  plugin ");
            }
            if (verbose) postResult(plugin->probeResult);
        }
    }
    auto result = factory.get();
    gPluginManager.addFactory(path, std::move(factory));
    addPlugins(*result);
    return result;
}

static bool isAbsolutePath(const std::string& path) {
    if (!path.empty() &&
        (path[0] == '/' || path[0] == '~' // Unix
#ifdef _WIN32
            || path[0] == '%' // environment variable
            || (path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) // drive
#endif
            )) return true;
    return false;
}

// resolves relative paths to an existing plugin in the canvas search paths or VST search paths.
// returns empty string on failure!
static std::string resolvePath(std::string path) {
    if (isAbsolutePath(path)) {
        return path; // success
    }
#ifdef _WIN32
    const char* ext = ".dll";
#elif defined(__APPLE__)
    const char* ext = ".vst";
#else
    const char* ext = ".so";
#endif
    if (path.find(".vst3") == std::string::npos && path.find(ext) == std::string::npos) {
        path += ext;
    }
    // otherwise try default VST paths
    for (auto& vstpath : getDefaultSearchPaths()) {
        auto result = vst::search(vstpath, path);
        if (!result.empty()) return result; // success
    }
    return std::string{}; // fail
}

// query a plugin by its key or file path and probe if necessary.
static const VSTPluginDesc* queryPlugin(std::string path) {
#ifdef _WIN32
    for (auto& c : path) {
        if (c == '\\') c = '/';
    }
#endif
    // query plugin
    auto desc = gPluginManager.findPlugin(path);
    if (!desc) {
        // try as file path 
        path = resolvePath(path);
        if (!path.empty() && !(desc = gPluginManager.findPlugin(path))) {
            // plugin descs might have been removed by 'search_clear'
            auto factory = gPluginManager.findFactory(path);
            if (factory) {
                addPlugins(*factory);
            }
            if (!(desc = gPluginManager.findPlugin(path))) {
                // finally probe plugin
                if (probePlugin(path, true)) {
                    // this fails if the module contains several plugins (so the path is not used as a key)
                    desc = gPluginManager.findPlugin(path);
                }
                else {
                    LOG_DEBUG("couldn't probe plugin");
                }
            }
        }
    }
    if (!desc) LOG_DEBUG("couldn't query plugin");
    return desc.get();
}

std::vector<vst::VSTPluginDesc *> searchPlugins(const std::string & path, bool verbose) {
    std::vector<vst::VSTPluginDesc*> results;
    LOG_VERBOSE("searching in '" << path << "'...");
    vst::search(path, [&](const std::string & absPath, const std::string &) {
        std::string pluginPath = absPath;
#ifdef _WIN32
        for (auto& c : pluginPath) {
            if (c == '\\') c = '/';
        }
#endif
        // check if module has already been loaded
        IVSTFactory* factory = gPluginManager.findFactory(pluginPath);
        if (factory) {
            // just post names of valid plugins
            auto plugins = factory->plugins();
            if (plugins.size() == 1) {
                auto& plugin = plugins[0];
                if (plugin->valid()) {
                    if (verbose) LOG_VERBOSE(pluginPath << " " << plugin->name);
                    results.push_back(plugin.get());
                }
            }
            else {
                if (verbose) LOG_VERBOSE(pluginPath);
                for (auto& plugin : factory->plugins()) {
                    if (plugin->valid()) {
                        if (verbose) LOG_VERBOSE("  " << plugin->name);
                        results.push_back(plugin.get());
                    }
                }
            }
            // (re)add plugins (in case they have been removed by 'search_clear'
            addPlugins(*factory);
        }
        else {
            // probe (will post results and add plugins)
            if ((factory = probePlugin(pluginPath, verbose))) {
                for (auto& plugin : factory->plugins()) {
                    if (plugin->valid()) {
                        results.push_back(plugin.get());
                    }
                }
            }
        }
    });
    LOG_VERBOSE("found " << results.size() << " plugin"
        << (results.size() == 1 ? "." : "s."));
    return results;
}

// VSTPluginListener

VSTPluginListener::VSTPluginListener(VSTPlugin &owner)
    : owner_(&owner) {}

struct ParamAutomatedData {
    VSTPlugin *owner;
    int32 index;
    float value;
};

// NOTE: in case we don't have a GUI thread we *could* get rid of nrtThreadID
// and just assume that std::this_thread::get_id() != rtThreadID_ means
// we're on the NRT thread - but I don't know if can be 100% sure about this,
// so let's play it safe.
void VSTPluginListener::parameterAutomated(int index, float value) {
    // RT thread
    if (std::this_thread::get_id() == owner_->rtThreadID_) {
#if 0
        // linked parameters automated by control busses or Ugens
        owner_->parameterAutomated(index, value);
#endif
    }
    // NRT thread
    else if (std::this_thread::get_id() == owner_->nrtThreadID_) {
        FifoMsg msg;
        auto data = new ParamAutomatedData{ owner_, index, value };
        msg.Set(owner_->mWorld, // world
            [](FifoMsg *msg) { // perform
                auto data = (ParamAutomatedData *)msg->mData;
                data->owner->parameterAutomated(data->index, data->value);
            }, [](FifoMsg *msg) { // free
                delete (ParamAutomatedData *)msg->mData;
            }, data); // data
        SendMsgToRT(owner_->mWorld, msg);
    }
#if VSTTHREADS
    // GUI thread (neither RT nor NRT thread) - push to queue
    else {
        std::lock_guard<std::mutex> guard(owner_->mutex_);
        owner_->paramQueue_.emplace_back(index, value);
    }
#endif
}

void VSTPluginListener::midiEvent(const VSTMidiEvent& midi) {
#if VSTTHREADS
    // check if we're on the realtime thread, otherwise ignore it
    if (std::this_thread::get_id() == owner_->rtThreadID_) {
#else
    {
#endif
        owner_->midiEvent(midi);
    }
}

void VSTPluginListener::sysexEvent(const VSTSysexEvent& sysex) {
#if VSTTHREADS
    // check if we're on the realtime thread, otherwise ignore it
    if (std::this_thread::get_id() == owner_->rtThreadID_) {
#else
    {
#endif
        owner_->sysexEvent(sysex);
    }
}

// VSTPlugin

VSTPlugin::VSTPlugin(){
    rtThreadID_ = std::this_thread::get_id();
    // LOG_DEBUG("RT thread ID: " << rtThreadID_);
    listener_ = std::make_unique<VSTPluginListener>(*this);

    numInChannels_ = in0(1);
    numOutChannels_ = numOutputs();
    parameterControlOnset_ = inChannelOnset_ + numInChannels_;
    numParameterControls_ = (int)(numInputs() - parameterControlOnset_) / 2;
    // LOG_DEBUG("num in: " << numInChannels_ << ", num out: " << numOutChannels_ << ", num controls: " << numParameterControls_);
    resizeBuffer();

    set_calc_function<VSTPlugin, &VSTPlugin::next>();

    runUnitCmds();
}

VSTPlugin::~VSTPlugin(){
    close();
    if (buf_) RTFree(mWorld, buf_);
    if (inBufVec_) RTFree(mWorld, inBufVec_);
    if (outBufVec_) RTFree(mWorld, outBufVec_);
    if (paramStates_) RTFree(mWorld, paramStates_);
    LOG_DEBUG("destroyed VSTPlugin");
}

IVSTPlugin *VSTPlugin::plugin() {
    return plugin_.get();
}

bool VSTPlugin::check(){
    if (plugin_) {
        return true;
    }
    else {
        LOG_WARNING("VSTPlugin: no plugin loaded!");
        return false;
    }
}

bool VSTPlugin::initialized() {
    return (initialized_ == MagicInitialized);
}

// Terrible hack to enable sending unit commands right after /s_new
// although the UGen constructor hasn't been called yet. See "runUnitCommand".
void VSTPlugin::queueUnitCmd(UnitCmdFunc fn, sc_msg_iter* args) {
    if (queued_ != MagicQueued) {
        unitCmdQueue_ = nullptr;
        queued_ = MagicQueued;
    }
    auto item = (UnitCmdQueueItem *)RTAlloc(mWorld, sizeof(UnitCmdQueueItem) + args->size);
    if (item) {
        item->next = nullptr;
        item->fn = fn;
        item->size = args->size;
        memcpy(item->data, args->data, args->size);
        // push to the back
        if (unitCmdQueue_) {
            auto tail = unitCmdQueue_;
            while (tail->next) tail = tail->next;
            tail->next = item;
        }
        else {
            unitCmdQueue_ = item;
        }
    }
}

void VSTPlugin::runUnitCmds() {
    if (queued_ == MagicQueued) {
        auto item = unitCmdQueue_;
        while (item){
            sc_msg_iter args(item->size, item->data);
            // swallow the first 3 arguments
            args.geti(); // node ID
            args.geti(); // ugen index
            args.gets(); // unit command name
            (item->fn)((Unit*)this, &args);
            auto next = item->next;
            RTFree(mWorld, item);
            item = next;
        }
    }
}

void VSTPlugin::resizeBuffer(){
    int blockSize = bufferSize();
    int nin = numInChannels_;
    int nout = numOutChannels_;
    bool fail = false;
    if (plugin_){
        nin = std::max<int>(nin, plugin_->getNumInputs());
        nout = std::max<int>(nout, plugin_->getNumOutputs());
    }
    // buffer
    {
        int bufSize = (nin + nout) * blockSize * sizeof(float);
        auto result = (float *)RTRealloc(mWorld, buf_, bufSize);
        if (result) {
            buf_ = result;
            memset(buf_, 0, bufSize);
        }
        else goto fail;
    }
    // input buffer array
    {
        if (nin > 0) {
            auto result = (const float**)RTRealloc(mWorld, inBufVec_, nin * sizeof(float*));
            if (result || nin == 0) {
                inBufVec_ = result;
                for (int i = 0; i < nin; ++i) {
                    inBufVec_[i] = &buf_[i * blockSize];
                }
            }
            else goto fail;
        }
        else {
            RTFree(mWorld, inBufVec_);
            inBufVec_ = nullptr;
        }
    }
    // output buffer array
    {
        if (nout > 0) {
            auto result = (float**)RTRealloc(mWorld, outBufVec_, nout * sizeof(float*));
            if (result) {
                outBufVec_ = result;
                for (int i = 0; i < nout; ++i) {
                    outBufVec_[i] = &buf_[(i + nin) * blockSize];
                }
            }
            else goto fail;
        }
        else {
            RTFree(mWorld, outBufVec_);
            outBufVec_ = nullptr;
        }
    }
    return;
fail:
    LOG_ERROR("RTRealloc failed!");
    RTFree(mWorld, buf_);
    RTFree(mWorld, inBufVec_);
    RTFree(mWorld, outBufVec_);
    buf_ = nullptr; inBufVec_ = nullptr; outBufVec_ = nullptr;
}

    // try to close the plugin in the NRT thread with an asynchronous command
void VSTPlugin::close() {
    if (plugin_) {
        auto cmdData = makeCmdData();
        if (!cmdData) {
            return;
        }
        // plugin, window and thread don't depend on VSTPlugin so they can be
        // safely moved to cmdData (which takes care of the actual closing)
        cmdData->plugin = std::move(plugin_);
        cmdData->window = std::move(window_);
#if VSTTHREADS
        cmdData->thread = std::move(thread_);
#endif
        doCmd(cmdData, [](World *world, void* data) {
            ((VSTPluginCmdData *)data)->close();
            return false; // done
        });
        window_ = nullptr;
        plugin_ = nullptr;
    }
}

void VSTPluginCmdData::close() {
    if (!plugin) return;
#if VSTTHREADS
    if (window) {
        plugin = nullptr; // release our plugin reference
        // terminate the message loop - will implicitly release the plugin in the GUI thread!
        // (some plugins expect to be released in the same thread where they have been created.)
        window->quit();
        // now join the thread
        if (thread.joinable()) {
            thread.join();
            LOG_DEBUG("thread joined");
        }
        // finally destroy the window
        window = nullptr;
    }
    else {
#else
    {
#endif
        // first destroy the window (if any)
        window = nullptr;
        // then release the plugin
        plugin = nullptr;
    }
    LOG_DEBUG("VST plugin closed");
}

bool cmdOpen(World *world, void* cmdData) {
    LOG_DEBUG("cmdOpen");
    // initialize GUI backend (if needed)
    auto data = (VSTPluginCmdData *)cmdData;
    data->threadID = std::this_thread::get_id();
    if (data->value) { // VST gui?
#ifdef __APPLE__
        LOG_WARNING("Warning: VST GUI not supported (yet) on macOS!");
        data->value = false;
#else
        static bool initialized = false;
        if (!initialized) {
            IVSTWindow::initialize();
            initialized = true;
        }
#endif
    }
    data->tryOpen();
    auto plugin = data->plugin.get();
    if (plugin) {
        auto owner = data->owner;
        plugin->suspend();
        // we only access immutable members of owner
        plugin->setSampleRate(owner->sampleRate());
        plugin->setBlockSize(owner->bufferSize());
        if (plugin->hasPrecision(VSTProcessPrecision::Single)) {
            plugin->setPrecision(VSTProcessPrecision::Single);
        }
        else {
            LOG_WARNING("VSTPlugin: plugin '" << plugin->getPluginName() << "' doesn't support single precision processing - bypassing!");
        }
        int nin = std::min<int>(plugin->getNumInputs(), owner->numInChannels());
        int nout = std::min<int>(plugin->getNumOutputs(), owner->numOutChannels());
        plugin->setNumSpeakers(nin, nout);
        plugin->resume();
    }
    return true;
}

bool cmdOpenDone(World *world, void* cmdData) {
    auto data = (VSTPluginCmdData *)cmdData;
    data->owner->doneOpen(*data);
    return false; // done
}

    // try to open the plugin in the NRT thread with an asynchronous command
void VSTPlugin::open(const char *path, bool gui) {
    LOG_DEBUG("open");
    if (isLoading_) {
        LOG_WARNING("already loading!");
        return;
    }
    close();
    if (plugin_) {
        LOG_ERROR("couldn't close current plugin!");
        return;
    }
    
    auto cmdData = makeCmdData(path);
    if (cmdData) {
        cmdData->value = gui;
        doCmd(cmdData, cmdOpen, cmdOpenDone);
        isLoading_ = true;
    }
}

void VSTPlugin::doneOpen(VSTPluginCmdData& cmd){
    LOG_DEBUG("doneOpen");
    isLoading_ = false;
    plugin_ = std::move(cmd.plugin);
    window_ = std::move(cmd.window);
    nrtThreadID_ = cmd.threadID;
    // LOG_DEBUG("NRT thread ID: " << nrtThreadID_);
#if VSTTHREADS
    thread_ = std::move(cmd.thread);
#endif
    if (plugin_){
        LOG_DEBUG("loaded " << cmd.buf);
        // receive events from plugin
        plugin_->setListener(listener_.get());
        resizeBuffer();
        // allocate arrays for parameter values/states
        int nParams = plugin_->getNumParameters();
        auto result = (Param *)RTRealloc(mWorld, paramStates_, nParams * sizeof(Param));
        if (result){
            paramStates_ = result;
            for (int i = 0; i < nParams; ++i) {
                paramStates_[i].value = std::numeric_limits<float>::quiet_NaN();
                paramStates_[i].bus = -1;
            }
        } else {
            RTFree(mWorld, paramStates_);
            paramStates_ = nullptr;
            LOG_ERROR("RTRealloc failed!");
        }
        // success, window
        float data[] = { 1.f, static_cast<float>(window_ != nullptr) };
        sendMsg("/vst_open", 2, data);
    } else {
        LOG_WARNING("VSTPlugin: couldn't load " << cmd.buf);
        sendMsg("/vst_open", 0);
    }
}

#if VSTTHREADS
using VSTPluginCmdPromise = std::promise<std::pair<std::shared_ptr<IVSTPlugin>, std::shared_ptr<IVSTWindow>>>;
void threadFunction(VSTPluginCmdPromise promise, const char *path);
#endif

void VSTPluginCmdData::tryOpen(){
#if VSTTHREADS
        // creates a new thread where the plugin is created and the message loop runs
    if (value){ // VST gui?
        VSTPluginCmdPromise promise;
        auto future = promise.get_future();
        LOG_DEBUG("started thread");
        thread = std::thread(threadFunction, std::move(promise), buf);
            // wait for thread to return the plugin and window
        auto result = future.get();
        LOG_DEBUG("got result from thread");
        plugin = result.first;
        window = result.second;
        if (!window) {
            thread.join(); // to avoid a crash in ~VSTPluginCmdData
        }
        return;
    }
#endif
        // create plugin in main thread
    auto desc = queryPlugin(buf);
    if (desc) {
        plugin = desc->create();
    }
#if !VSTTHREADS
        // create and setup GUI window in main thread (if needed)
    if (plugin && plugin->hasEditor() && value){
        window = IVSTWindow::create(*plugin);
        if (window){
            window->setTitle(plugin->getPluginName());
            int left, top, right, bottom;
            plugin->getEditorRect(left, top, right, bottom);
            window->setGeometry(left, top, right, bottom);
            // don't open the editor on macOS (see VSTWindowCocoa.mm)
#ifndef __APPLE__
            plugin->openEditor(window->getHandle());
#endif
        }
    }
#endif
}

#if VSTTHREADS
void threadFunction(VSTPluginCmdPromise promise, const char *path){
    std::shared_ptr<IVSTPlugin> plugin;
    auto desc = queryPlugin(path);
    if (desc) {
        plugin = desc->create();
    }
    if (!plugin){
            // signal other thread
        promise.set_value({ nullptr, nullptr });
        return;
    }
        // create GUI window (if needed)
    std::shared_ptr<IVSTWindow> window;
    if (plugin->hasEditor()){
        window = IVSTWindow::create(*plugin);
    }
        // return plugin and window to other thread
        // (but keep references in the GUI thread)
    promise.set_value({ plugin, window });
        // setup GUI window (if any)
    if (window){
        window->setTitle(plugin->getPluginName());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        window->setGeometry(left, top, right, bottom);

        plugin->openEditor(window->getHandle());
            // run the event loop until it gets a quit message 
            // (the editor will we closed implicitly)
        LOG_DEBUG("start message loop");
        window->run();
        LOG_DEBUG("end message loop");
    }
}
#endif

bool cmdShowEditor(World *world, void *cmdData) {
    auto data = (VSTPluginCmdData *)cmdData;
    if (data->value) {
        data->window->bringToTop();
    }
    else {
        data->window->hide();
    }
    return false; // done
}

void VSTPlugin::showEditor(bool show) {
    if (plugin_ && window_) {
        auto cmdData = makeCmdData();
        if (cmdData) {
            cmdData->window = window_;
            cmdData->value = show;
            doCmd(cmdData, cmdShowEditor);
        }
    }
}

// some plugins crash when being reset reset
// in the NRT thread. we let the user choose
// and add a big fat warning in the help file.
bool cmdReset(World *world, void *cmdData) {
    auto data = (VSTPluginCmdData *)cmdData;
    data->owner->plugin()->suspend();
    data->owner->plugin()->resume();
    return false; // done
}

void VSTPlugin::reset(bool async) {
    if (check()) {
        if (async) {
            // reset in the NRT thread (unsafe)
            doCmd(makeCmdData(), cmdReset);
        }
        else {
            // reset in the RT thread (safe)
            plugin_->suspend();
            plugin_->resume();
        }
    }
}

// perform routine
void VSTPlugin::next(int inNumSamples) {
    if (!buf_) {
        // only if RT memory methods failed in resizeBuffer()
        (*ft->fClearUnitOutputs)(this, inNumSamples);
    }
    int nin = numInChannels_;
    int nout = numOutChannels_;
    bool bypass = in0(0);
    int offset = 0;
    // disable reset for realtime-safety reasons
#if 0
    // only reset plugin when bypass changed from true to false
    if (plugin_ && !bypass && (bypass != bypass_)) {
        reset();
    }
    bypass_ = bypass;
#endif
    // setup pointer arrays:
    for (int i = 0; i < nin; ++i) {
        inBufVec_[i] = in(i + inChannelOnset_);
    }
    for (int i = 0; i < nout; ++i) {
        outBufVec_[i] = out(i);
    }

    if (plugin_ && !bypass && plugin_->hasPrecision(VSTProcessPrecision::Single)) {
        if (paramStates_) {
            // update parameters from mapped control busses
            int nparam = plugin_->getNumParameters();
            for (int i = 0; i < nparam; ++i) {
                int bus = paramStates_[i].bus;
                if (bus >= 0) {
                    float value = readControlBus(bus);
                    if (value != paramStates_[i].value) {
                        plugin_->setParameter(i, value);
                        paramStates_[i].value = value;
                    }
                }
            }
            // update parameters from UGen inputs
            for (int i = 0; i < numParameterControls_; ++i) {
                int k = 2 * i + parameterControlOnset_;
                int index = in0(k);
                float value = in0(k + 1);
                // only if index is not out of range and the param is not mapped to a bus
                if (index >= 0 && index < nparam && paramStates_[index].bus < 0
                    && paramStates_[index].value != value)
                {
                    plugin_->setParameter(index, value);
                    paramStates_[index].value = value;
                }
            }
        }
        // process
        plugin_->process((const float **)inBufVec_, outBufVec_, inNumSamples);
        offset = plugin_->getNumOutputs();

#if VSTTHREADS
        // send parameter automation notification posted from the GUI thread.
        // we assume this is only possible if we have a VST editor window.
        // try_lock() won't block the audio thread and we don't mind if notifications
        // will be delayed if try_lock() fails (which happens rarely in practice).
        if (window_ && mutex_.try_lock()) {
            std::vector<std::pair<int, float>> queue;
            queue.swap(paramQueue_);
            mutex_.unlock();
            for (auto& p : queue) {
                parameterAutomated(p.first, p.second);
            }
        }
#endif
    }
    else {
        // bypass (copy input to output)
        int n = std::min(nin, nout);
        for (int i = 0; i < n; ++i) {
            Copy(inNumSamples, outBufVec_[i], (float *)inBufVec_[i]);
        }
        offset = n;
    }
    // zero remaining outlets
    for (int i = offset; i < nout; ++i) {
        Fill(inNumSamples, outBufVec_[i], 0.f);
    }
}

bool cmdSetParam(World *world, void *cmdData) {
    auto data = (ParamCmdData *)cmdData;
    auto index = data->index;
    if (data->display[0]) {
        data->owner->plugin()->setParameter(index, data->display);
    }
    else {
        data->owner->plugin()->setParameter(index, data->value);
    }
    return true;
}

bool cmdSetParamDone(World *world, void *cmdData) {
    auto data = (ParamCmdData *)cmdData;
    data->owner->setParamDone(data->index);
    return false; // done
}

void VSTPlugin::setParam(int32 index, float value) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            auto data = (ParamCmdData *)RTAlloc(mWorld, sizeof(ParamCmdData));
            if (data) {
                data->owner = this;
                data->index = index;
                data->value = value;
                data->display[0] = 0;
                doCmd(data, cmdSetParam, cmdSetParamDone);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPlugin::setParam(int32 index, const char *display) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            auto len = strlen(display) + 1;
            auto data = (ParamCmdData *)RTAlloc(mWorld, sizeof(ParamCmdData) + len);
            if (data) {
                data->owner = this;
                data->index = index;
                data->value = 0;
                memcpy(data->display, display, len);
                doCmd(data, cmdSetParam, cmdSetParamDone);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPlugin::setParamDone(int32 index) {
    paramStates_[index].value = plugin_->getParameter(index);
    paramStates_[index].bus = -1; // invalidate bus num
    sendParameter(index);
}

void VSTPlugin::queryParams(int32 index, int32 count) {
    if (check()) {
        int32 nparam = plugin_->getNumParameters();
        if (index >= 0 && index < nparam) {
            count = std::min<int32>(count, nparam - index);
            for (int i = 0; i < count; ++i) {
                sendParameter(index + i);
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPlugin::getParam(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            float value = plugin_->getParameter(index);
            sendMsg("/vst_set", value);
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPlugin::getParams(int32 index, int32 count) {
    if (check()) {
        int32 nparam = plugin_->getNumParameters();
        if (index >= 0 && index < nparam) {
            count = std::min<int32>(count, nparam - index);
            const int bufsize = count + 1;
            float *buf = (float *)RTAlloc(mWorld, sizeof(float) * bufsize);
            if (buf) {
                buf[0] = count;
                for (int i = 0; i < count; ++i) {
                    float value = plugin_->getParameter(i + index);
                    buf[i + 1] = value;
                }
                sendMsg("/vst_setn", bufsize, buf);
                RTFree(mWorld, buf);
            }
            else {
                LOG_WARNING("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPlugin::mapParam(int32 index, int32 bus) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            paramStates_[index].bus = bus;
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPlugin::unmapParam(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            paramStates_[index].bus = -1;
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

// program/bank
bool cmdSetProgram(World *world, void *cmdData) {
    auto data = (VSTPluginCmdData *)cmdData;
    data->owner->plugin()->setProgram(data->value);
    return true;
}

bool cmdSetProgramDone(World *world, void *cmdData) {
    auto data = (VSTPluginCmdData *)cmdData;
    data->owner->sendMsg("/vst_program_index", data->owner->plugin()->getProgram());
    return false; // done
}

void VSTPlugin::setProgram(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumPrograms()) {
            auto data = makeCmdData();
            if (data) {
                data->value = index;
                doCmd(data, cmdSetProgram, cmdSetProgramDone);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: program number " << index << " out of range!");
        }
    }
}
void VSTPlugin::setProgramName(const char *name) {
    if (check()) {
        plugin_->setProgramName(name);
        sendCurrentProgramName();
    }
}

void VSTPlugin::queryPrograms(int32 index, int32 count) {
    if (check()) {
        int32 nprogram = plugin_->getNumPrograms();
        if (index >= 0 && index < nprogram) {
            count = std::min<int32>(count, nprogram - index);
#if 1
            for (int i = 0; i < count; ++i) {
                sendProgramName(index + i);
            }
#else
            auto old = plugin_->getProgram();
            bool changed;
            for (int i = 0; i < count; ++i) {
                changed = sendProgramName(index + i);
            }
            if (changed) {
                plugin_->setProgram(old);
            }
#endif
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

template<bool bank>
bool cmdReadPreset(World* world, void* cmdData) {
    auto data = (InfoCmdData*)cmdData;
    auto plugin = data->owner->plugin();
    bool result;
    if (data->bufnum < 0) {
        // from file
        if (bank)
            result = plugin->readBankFile(data->path);
        else
            result = plugin->readProgramFile(data->path);
    }
    else {
        // from buffer
        std::string presetData;
        auto buf = World_GetNRTBuf(world, data->bufnum);
        writeBuffer(buf, presetData);
        if (bank)
            result = plugin->readBankData(presetData);
        else
            result = plugin->readProgramData(presetData);
    }
    data->flags = result;
    return true;
}

template<bool bank>
bool cmdReadPresetDone(World *world, void *cmdData){
    auto data = (InfoCmdData *)cmdData;
    auto owner = data->owner;
    if (bank) {
        owner->sendMsg("/vst_bank_read", data->flags);
        // a bank change also sets the current program number!
        owner->sendMsg("/vst_program_index", owner->plugin()->getProgram());
    }
    else {
        owner->sendMsg("/vst_program_read", data->flags);
    }
    // the program name has most likely changed
    owner->sendCurrentProgramName();
    return false; // done
}

void VSTPlugin::readProgram(const char *path){
    if (check()){
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            snprintf(data->path, sizeof(data->path), "%s", path);
            doCmd(data, cmdReadPreset<false>, cmdReadPresetDone<false>);
        }
    }
}

void VSTPlugin::readProgram(int32 buf) {
    if (check()) {
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            data->bufnum = buf;
            doCmd(data, cmdReadPreset<false>, cmdReadPresetDone<false>);
        }
    }
}

void VSTPlugin::readBank(const char *path) {
    if (check()) {
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            snprintf(data->path, sizeof(data->path), "%s", path);
            doCmd(data, cmdReadPreset<true>, cmdReadPresetDone<true>);
        }
    }
}

void VSTPlugin::readBank(int32 buf) {
    if (check()) {
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            data->bufnum = buf;
            doCmd(data, cmdReadPreset<true>, cmdReadPresetDone<true>);
        }
    }
}

template<bool bank>
bool cmdWritePreset(World *world, void *cmdData){
    auto data = (InfoCmdData *)cmdData;
    auto plugin = data->owner->plugin();
    bool result = true;
    if (data->bufnum < 0) {
        // to file (LATER report failure)
        if (bank)
            plugin->writeBankFile(data->path);
        else
            plugin->writeProgramFile(data->path);
    }
    else {
        // to buffer
        std::string presetData;
        if (bank)
            plugin->writeBankData(presetData);
        else
            plugin->writeProgramData(presetData);
        // LATER get error from method
        if (!presetData.empty()) {
            auto buf = World_GetNRTBuf(world, data->bufnum);
            data->freeData = buf->data; // to be freed in stage 4
            allocReadBuffer(buf, presetData);
        }
        else {
            result = false;
        }
    }
    data->flags = result;
    return true;
}

template<bool bank>
bool cmdWritePresetDone(World *world, void *cmdData){
    auto data = (InfoCmdData *)cmdData;
    data->owner->sendMsg(bank ? "/vst_bank_write" : "/vst_program_write", data->flags);
    return true; // continue
}

void VSTPlugin::writeProgram(const char *path) {
    if (check()) {
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            snprintf(data->path, sizeof(data->path), "%s", path);
            doCmd(data, cmdWritePreset<false>, cmdWritePresetDone<false>, InfoCmdData::nrtFree);
        }
    }
}

void VSTPlugin::writeProgram(int32 buf) {
    if (check()) {
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            data->bufnum = buf;
            doCmd(data, cmdWritePreset<false>, cmdWritePresetDone<false>, InfoCmdData::nrtFree);
        }
    }
}

void VSTPlugin::writeBank(const char *path) {
    if (check()) {
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            snprintf(data->path, sizeof(data->path), "%s", path);
            doCmd(data, cmdWritePreset<true>, cmdWritePresetDone<true>, InfoCmdData::nrtFree);
        }
    }
}

void VSTPlugin::writeBank(int32 buf) {
    if (check()) {
        auto data = InfoCmdData::create(mWorld);
        if (data) {
            data->owner = this;
            data->bufnum = buf;
            doCmd(data, cmdWritePreset<true>, cmdWritePresetDone<true>, InfoCmdData::nrtFree);
        }
    }
}

// midi
void VSTPlugin::sendMidiMsg(int32 status, int32 data1, int32 data2) {
    if (check()) {
        plugin_->sendMidiEvent(VSTMidiEvent(status, data1, data2));
    }
}
void VSTPlugin::sendSysexMsg(const char *data, int32 n) {
    if (check()) {
        plugin_->sendSysexEvent(VSTSysexEvent(data, n));
    }
}
// transport
void VSTPlugin::setTempo(float bpm) {
    if (check()) {
        plugin_->setTempoBPM(bpm);
    }
}
void VSTPlugin::setTimeSig(int32 num, int32 denom) {
    if (check()) {
        plugin_->setTimeSignature(num, denom);
    }
}
void VSTPlugin::setTransportPlaying(bool play) {
    if (check()) {
        plugin_->setTransportPlaying(play);
    }
}
void VSTPlugin::setTransportPos(float pos) {
    if (check()) {
        plugin_->setTransportPosition(pos);
    }
}
void VSTPlugin::getTransportPos() {
    if (check()) {
        float f = plugin_->getTransportPosition();
        sendMsg("/vst_transport", f);
    }
}

// advanced

void VSTPlugin::canDo(const char *what) {
    if (check()) {
        auto result = plugin_->canDo(what);
        sendMsg("/vst_can_do", (float)result);
    }
}

bool cmdVendorSpecific(World *world, void *cmdData) {
    auto data = (VendorCmdData *)cmdData;
    auto result = data->owner->plugin()->vendorSpecific(data->index, data->value, data->data, data->opt);
    data->index = result; // save result
    return true;
}

bool cmdVendorSpecificDone(World *world, void *cmdData) {
    auto data = (VendorCmdData *)cmdData;
    data->owner->sendMsg("/vst_vendor_method", (float)(data->index));
    return false; // done
}

void VSTPlugin::vendorSpecific(int32 index, int32 value, size_t size, const char *data, float opt, bool async) {
    if (check()) {
        if (async) {
            auto cmdData = (VendorCmdData *)RTAlloc(mWorld, sizeof(VendorCmdData) + size);
            if (cmdData) {
                cmdData->owner = this;
                cmdData->index = index;
                cmdData->value = value;
                cmdData->opt = opt;
                cmdData->size = size;
                memcpy(cmdData->data, data, size);
                doCmd(cmdData, cmdVendorSpecific, cmdVendorSpecificDone);
            }
            else {
                LOG_WARNING("RTAlloc failed!");
            }
        }
        else {
            auto result = plugin_->vendorSpecific(index, value, (void *)data, opt);
            sendMsg("/vst_vendor_method", (float)result);
        }
    }
}

/*** helper methods ***/

float VSTPlugin::readControlBus(int32 num) {
    if (num >= 0 && num < mWorld->mNumControlBusChannels) {
#define unit this
        ACQUIRE_BUS_CONTROL(num);
        float value = mWorld->mControlBus[num];
        RELEASE_BUS_CONTROL(num);
        return value;
#undef unit
    }
    else {
        return 0.f;
    }
}

// unchecked
bool VSTPlugin::sendProgramName(int32 num) {
    const int maxSize = 64;
    float buf[maxSize];
    bool changed = false;
    auto name = plugin_->getProgramNameIndexed(num);
#if 0
    // some old plugins don't support indexed program name lookup
    if (name.empty()) {
        plugin_->setProgram(num);
        name = plugin_->getProgramName();
        changed = true;
    }
#endif
    // msg format: index, len, characters...
    buf[0] = num;
    int size = string2floatArray(name, buf + 1, maxSize - 1);
    sendMsg("/vst_program", size + 1, buf);
    return changed;
}

void VSTPlugin::sendCurrentProgramName() {
    const int maxSize = 64;
    float buf[maxSize];
    // msg format: index, len, characters...
    buf[0] = plugin_->getProgram();
    int size = string2floatArray(plugin_->getProgramName(), buf + 1, maxSize - 1);
    sendMsg("/vst_program", size + 1, buf);
}

// unchecked
void VSTPlugin::sendParameter(int32 index) {
    const int maxSize = 64;
    float buf[maxSize];
    // msg format: index, value, display length, display chars...
    buf[0] = index;
    buf[1] = plugin_->getParameter(index);
    int size = string2floatArray(plugin_->getParameterDisplay(index), buf + 2, maxSize - 2);
    sendMsg("/vst_param", size + 2, buf);
}

void VSTPlugin::parameterAutomated(int32 index, float value) {
    sendParameter(index);
    float buf[2] = { (float)index, value };
    sendMsg("/vst_auto", 2, buf);
}

void VSTPlugin::midiEvent(const VSTMidiEvent& midi) {
    float buf[3];
    // we don't want negative values here
    buf[0] = (unsigned char) midi.data[0];
    buf[1] = (unsigned char) midi.data[1];
    buf[2] = (unsigned char) midi.data[2];
    sendMsg("/vst_midi", 3, buf);
}

void VSTPlugin::sysexEvent(const VSTSysexEvent& sysex) {
    auto& data = sysex.data;
    int size = data.size();
    if ((size * sizeof(float)) > MAX_OSC_PACKET_SIZE) {
        LOG_WARNING("sysex message (" << size << " bytes) too large for UDP packet - dropped!");
        return;
    }
    float *buf = (float *)RTAlloc(mWorld, size * sizeof(float));
    if (buf) {
        for (int i = 0; i < size; ++i) {
            // no need to cast to unsigned because SC's Int8Array is signed anyway
            buf[i] = data[i];
        }
        sendMsg("/vst_sysex", size, buf);
        RTFree(mWorld, buf);
    }
    else {
        LOG_WARNING("RTAlloc failed!");
    }
}

void VSTPlugin::sendMsg(const char *cmd, float f) {
    // LOG_DEBUG("sending msg: " << cmd);
    SendNodeReply(&mParent->mNode, mParentIndex, cmd, 1, &f);
}

void VSTPlugin::sendMsg(const char *cmd, int n, const float *data) {
    // LOG_DEBUG("sending msg: " << cmd);
    SendNodeReply(&mParent->mNode, mParentIndex, cmd, n, data);
}

VSTPluginCmdData* VSTPlugin::makeCmdData(const char *data, size_t size){
    auto *cmdData = (VSTPluginCmdData *)RTAlloc(mWorld, sizeof(VSTPluginCmdData) + size);
    if (cmdData){
        new (cmdData) VSTPluginCmdData();
        cmdData->owner = this;
        if (data){
            memcpy(cmdData->buf, data, size);
        }
        cmdData->size = size; // independent from data
    }
    else {
        LOG_ERROR("RTAlloc failed!");
    }
    return cmdData;
}

VSTPluginCmdData* VSTPlugin::makeCmdData(const char *path){
    size_t len = path ? (strlen(path) + 1) : 0;
    return makeCmdData(path, len);
}

VSTPluginCmdData* VSTPlugin::makeCmdData() {
    return makeCmdData(nullptr, 0);
}

void cmdRTfree(World *world, void * cmdData) {
    if (cmdData) {
        RTFree(world, cmdData);
        // LOG_DEBUG("cmdRTfree!");
    }
}

// 'clean' version for non-POD data
template<typename T>
void cmdRTfree(World *world, void * cmdData) {
    if (cmdData) {
        ((T*)cmdData)->~T(); // should do nothing! (let's just be nice here)
        RTFree(world, cmdData);
        // LOG_DEBUG("cmdRTfree!");
    }
}

template<typename T>
void VSTPlugin::doCmd(T *cmdData, AsyncStageFn stage2,
    AsyncStageFn stage3, AsyncStageFn stage4) {
    // so we don't have to always check the return value of makeCmdData
    if (cmdData) {
        DoAsynchronousCommand(mWorld,
            0, 0, cmdData, stage2, stage3, stage4, cmdRTfree<T>, 0, 0);
    }
}

/*** unit command callbacks ***/

void vst_open(VSTPlugin *unit, sc_msg_iter *args) {
    const char *path = args->gets();
    auto gui = args->geti();
    if (path) {
        unit->open(path, gui);
    }
    else {
        LOG_WARNING("vst_open: expecting string argument!");
    }
}

void vst_close(VSTPlugin *unit, sc_msg_iter *args) {	
    unit->close();
}

void vst_reset(VSTPlugin *unit, sc_msg_iter *args) {	
    bool async = args->geti();
    unit->reset(async);
}

void vst_vis(VSTPlugin* unit, sc_msg_iter *args) {
    bool show = args->geti();
    unit->showEditor(show);
}

// set parameters given as pairs of index and value
void vst_set(VSTPlugin* unit, sc_msg_iter *args) {
    auto vst = unit;
    if (vst->check()) {
        while (args->remain() > 0) {
            int32 index = args->geti();
            if (args->remain() > 0 && args->nextTag() == 's') {
                vst->setParam(index, args->gets());
            }
            else {
                vst->setParam(index, args->getf());
            }
        }
    }
}

// set parameters given as triples of index, count and values
void vst_setn(VSTPlugin* unit, sc_msg_iter *args) {
    auto vst = unit;
    if (vst->check()) {
        int nparam = vst->plugin()->getNumParameters();
        while (args->remain() > 0) {
            int32 index = args->geti();
            int32 count = args->geti();
            for (int i = 0; i < count && args->remain() > 0; ++i) {
                if (args->nextTag() == 's') {
                    vst->setParam(index + i, args->gets());
                }
                else {
                    vst->setParam(index + i, args->getf());
                }
            }
        }
    }
}

// query parameters starting from index (values + displays)
void vst_param_query(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = args->geti();
    int32 count = args->geti();
    unit->queryParams(index, count);
}

// get a single parameter at index (only value)
void vst_get(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = args->geti(-1);
    unit->getParam(index);
}

// get a number of parameters starting from index (only values)
void vst_getn(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = args->geti();
    int32 count = args->geti();
    unit->getParams(index, count);
}

// map parameters to control busses
void vst_map(VSTPlugin* unit, sc_msg_iter *args) {
    auto vst = unit;
    if (vst->check()) {
        int nparam = vst->plugin()->getNumParameters();
        while (args->remain() > 0) {
            int32 index = args->geti();
            int32 bus = args->geti(-1);
            int32 numChannels = args->geti();
            for (int i = 0; i < numChannels; ++i) {
                int32 idx = index + i;
                if (idx >= 0 && idx < nparam) {
                    vst->mapParam(idx, bus + i);
                }
            }
        }
    }
}

// unmap parameters from control busses
void vst_unmap(VSTPlugin* unit, sc_msg_iter *args) {
    auto vst = unit;
    if (vst->check()) {
        int nparam = vst->plugin()->getNumParameters();
        if (args->remain() > 0) {
            do {
                int32 index = args->geti();
                if (index >= 0 && index < nparam) {
                    vst->unmapParam(index);
                }
            } while (args->remain() > 0);
        }
        else {
            // unmap all parameters:
            for (int i = 0; i < nparam; ++i) {
                vst->unmapParam(i);
            }
        }
        
    }
}

void vst_program_set(VSTPlugin *unit, sc_msg_iter *args) {	
    int32 index = args->geti();
    unit->setProgram(index);
}

// query parameters (values + displays) starting from index
void vst_program_query(VSTPlugin *unit, sc_msg_iter *args) {	
    int32 index = args->geti();
    int32 count = args->geti();
    unit->queryPrograms(index, count);
}

void vst_program_name(VSTPlugin* unit, sc_msg_iter *args) {
    const char *name = args->gets();
    if (name) {
        unit->setProgramName(name);
    }
    else {
        LOG_WARNING("vst_program_name: expecting string argument!");
    }
}

void vst_program_read(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        unit->readProgram(args->gets()); // file name
    }
    else {
        unit->readProgram(args->geti()); // buf num
    }
}

void vst_program_write(VSTPlugin *unit, sc_msg_iter *args) {	
    if (args->nextTag() == 's') {
        unit->writeProgram(args->gets()); // file name
    }
    else {
        unit->writeProgram(args->geti()); // buf num
    }
}

void vst_bank_read(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        unit->readBank(args->gets()); // file name
    }
    else {
        unit->readBank(args->geti()); // buf num
    }
}

void vst_bank_write(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        unit->writeBank(args->gets()); // file name
    }
    else {
        unit->writeBank(args->geti()); // buf num
    }
}

void vst_midi_msg(VSTPlugin* unit, sc_msg_iter *args) {
    char data[4];
    int32 len = args->getbsize();
    if (len > 4) {
        LOG_WARNING("vst_midi_msg: midi message too long (" << len << " bytes)");
    }
    args->getb(data, len);
    unit->sendMidiMsg(data[0], data[1], data[2]);
}

void vst_midi_sysex(VSTPlugin* unit, sc_msg_iter *args) {
    int len = args->getbsize();
    if (len > 0) {
        // LATER avoid unnecessary copying
        char *buf = (char *)RTAlloc(unit->mWorld, len);
        if (buf) {
            args->getb(buf, len);
            unit->sendSysexMsg(buf, len);
            RTFree(unit->mWorld, buf);
        }
        else {
            LOG_ERROR("vst_midi_sysex: RTAlloc failed!");
        }
    }
    else {
        LOG_WARNING("vst_midi_sysex: no data!");
    }
}

void vst_tempo(VSTPlugin* unit, sc_msg_iter *args) {
    float bpm = args->getf();
    unit->setTempo(bpm);
}

void vst_time_sig(VSTPlugin* unit, sc_msg_iter *args) {
    int32 num = args->geti();
    int32 denom = args->geti();
    unit->setTimeSig(num, denom);
}

void vst_transport_play(VSTPlugin* unit, sc_msg_iter *args) {
    int play = args->geti();
    unit->setTransportPlaying(play);
}

void vst_transport_set(VSTPlugin* unit, sc_msg_iter *args) {
    float pos = args->getf();
    unit->setTransportPos(pos);
}

void vst_transport_get(VSTPlugin* unit, sc_msg_iter *args) {
    unit->getTransportPos();
}

void vst_can_do(VSTPlugin* unit, sc_msg_iter *args) {
    const char* what = args->gets();
    if (what) {
        unit->canDo(what);
    }
}

void vst_vendor_method(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = args->geti();
    int32 value = args->geti(); // sc_msg_iter doesn't support 64bit ints...
    int32 size = args->getbsize();
    char *data = nullptr;
    if (size > 0) {
        data = (char *)RTAlloc(unit->mWorld, size);
        if (data) {
            args->getb(data, size);
        }
        else {
            LOG_ERROR("RTAlloc failed!");
            return;
        }
    }
    float opt = args->getf();
    bool async = args->geti();
    unit->vendorSpecific(index, value, size, data, opt, async);
    if (data) {
        RTFree(unit->mWorld, data);
    }
}

/*** plugin command callbacks ***/

// recursively search directories for VST plugins.
bool cmdSearch(World *inWorld, void* cmdData) {
    auto data = (InfoCmdData *)cmdData;
    std::vector<vst::VSTPluginDesc*> plugins;
    bool useDefault = data->flags & 1;
    bool verbose = (data->flags >> 1) & 1;
    std::vector<std::string> searchPaths;
    bool local = data->bufnum < 0;
    auto size = data->size;
    auto ptr = data->buf;
    auto onset = ptr;
    // file paths are seperated by 0
    while (size--) {
        if (*ptr++ == '\0') {
            auto diff = ptr - onset;
            searchPaths.emplace_back(onset, diff - 1); // don't store '\0'!
            onset = ptr;
        }
    }
    // use default search paths?
    if (useDefault) {
        for (auto& path : getDefaultSearchPaths()) {
            searchPaths.push_back(path);
        }
    }
    // search for plugins
    for (auto& path : searchPaths) {
        auto result = searchPlugins(path, verbose);
        plugins.insert(plugins.end(), result.begin(), result.end());
    }
    // write new info to file (only for local Servers) or buffer
    if (local) {
        // write to file
        std::ofstream file(data->path);
        if (file.is_open()) {
            LOG_DEBUG("writing plugin info to file");
            for (auto plugin : plugins) {
                file << makeKey(*plugin) << "\t";
                plugin->serialize(file);
                file << "\n"; // seperate plugins with newlines
            }
        }
        else {
            LOG_ERROR("couldn't write plugin info file '" << data->path << "'!");
        }
    }
    else {
        // write to buffer
        auto buf = World_GetNRTBuf(inWorld, data->bufnum);
        data->freeData = buf->data; // to be freed in stage 4
        std::stringstream ss;
        LOG_DEBUG("writing plugin info to buffer");
        for (auto plugin : plugins) {
            ss << makeKey(*plugin) << "\t";
            plugin->serialize(ss);
            ss << "\n"; // seperate plugins with newlines
        }
        allocReadBuffer(buf, ss.str());
    }
    return true;
}

bool cmdSearchDone(World *inWorld, void *cmdData) {
    auto data = (InfoCmdData*)cmdData;
    if (data->bufnum >= 0)
        syncBuffer(inWorld, data->bufnum);
    gSearching = false;
    // LOG_DEBUG("search done!");
    return true;
}

void vst_search(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    if (gSearching) {
        LOG_WARNING("already searching!");
        return;
    }
    int32 bufnum = -1;
    const char* filename = nullptr;
    // flags (useDefault, verbose, etc.)
    int flags = args->geti();
    // temp file or buffer to store the search results
    if (args->nextTag() == 's') {
        filename = args->gets();
    }
    else {
        bufnum = args->geti();
        if (bufnum < 0 || bufnum >= inWorld->mNumSndBufs) {
            LOG_ERROR("vst_search: bad bufnum " << bufnum);
            return;
        }
    }
    // collect optional search paths
    std::pair<const char *, size_t> paths[64];
    int numPaths = 0;
    auto pathLen = 0;
    while (args->remain() && numPaths < 64) {
        auto s = args->gets();
        if (s) {
            auto len = strlen(s) + 1; // include terminating '\0'
            paths[numPaths].first = s;
            paths[numPaths].second = len;
            pathLen += len;
            ++numPaths;
        }
    }

    auto data = InfoCmdData::create(inWorld, pathLen);
    if (data) {
        data->flags = flags;
        data->bufnum = bufnum;
        if (filename) {
            snprintf(data->path, sizeof(data->path), "%s", filename);
        }
        else {
            data->path[0] = '\0';
        }
        // now copy search paths into a single buffer (separated by '\0')
        data->size = pathLen;
        auto ptr = data->buf;
        for (int i = 0; i < numPaths; ++i) {
            auto s = paths[i].first;
            auto len = paths[i].second;
            memcpy(ptr, s, len);
            ptr += len;
        }
        // LOG_DEBUG("start search");
        DoAsynchronousCommand(inWorld, replyAddr, "vst_search",
            data, cmdSearch, cmdSearchDone, InfoCmdData::nrtFree, cmdRTfree, 0, 0);
        gSearching = true;
    }
}

void vst_clear(World* inWorld, void* inUserData, struct sc_msg_iter* args, void* replyAddr) {
    if (!gSearching) {
        DoAsynchronousCommand(inWorld, 0, 0, 0, [](World*, void*) {
            gPluginManager.clearPlugins();
            return false;
        }, 0, 0, cmdRTfree, 0, 0);
    }
    else {
        LOG_WARNING("can't clear while searching!");
    }
}

// query plugin info
bool cmdProbe(World *inWorld, void *cmdData) {
    auto data = (InfoCmdData *)cmdData;
    auto bufnum = data->bufnum;
    auto desc = queryPlugin(data->buf);
    // write info to file or buffer
    if (desc){
        if (bufnum < 0) {
            // write to file
            std::ofstream file(data->path);
            if (file.is_open()) {
                file << makeKey(*desc) << "\t";
                desc->serialize(file);
            }
            else {
                LOG_ERROR("couldn't write plugin info file '" << data->path << "'!");
            }
        }
        else {
            // write to buffer
            auto buf = World_GetNRTBuf(inWorld, data->bufnum);
            data->freeData = buf->data; // to be freed in stage 4
            std::stringstream ss;
            LOG_DEBUG("writing plugin info to buffer");
            ss << makeKey(*desc) << "\t";
            desc->serialize(ss);
            allocReadBuffer(buf, ss.str());
        }
        return true;
    }
    else {
        return false;
    }
}

bool cmdProbeDone(World* inWorld, void* cmdData) {
    auto data = (InfoCmdData*)cmdData;
    if (data->bufnum >= 0)
        syncBuffer(inWorld, data->bufnum);
    // LOG_DEBUG("probe done!");
    return true;
}

void vst_probe(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    if (gSearching) {
        LOG_WARNING("currently searching!");
        return;
    }
    if (args->nextTag() != 's') {
        LOG_ERROR("first argument to 'vst_probe' must be a string (plugin path)!");
        return;
    }
    auto path = args->gets(); // plugin path
    auto size = strlen(path) + 1;

    auto data = InfoCmdData::create(inWorld, size);
    if (data) {
        // temp file or buffer to store the plugin info
        if (args->nextTag() == 's') {
            auto filename = args->gets();
            snprintf(data->path, sizeof(data->path), "%s", filename);
            data->bufnum = -1;
        }
        else {
            auto bufnum = args->geti();
            if (bufnum < 0 || bufnum >= inWorld->mNumSndBufs) {
                LOG_ERROR("vst_search: bad bufnum " << bufnum);
                return;
            }
            data->bufnum = bufnum;
            data->path[0] = '\0';
        }

        memcpy(data->buf, path, size); // store plugin path

        DoAsynchronousCommand(inWorld, replyAddr, "vst_probe",
            data, cmdProbe, cmdProbeDone, InfoCmdData::nrtFree, cmdRTfree, 0, 0);
    }
}

/*** plugin entry point ***/

void VSTPlugin_Ctor(VSTPlugin* unit){
    new(unit)VSTPlugin();
}

void VSTPlugin_Dtor(VSTPlugin* unit){
    unit->~VSTPlugin();
}

using VSTUnitCmdFunc = void (*)(VSTPlugin*, sc_msg_iter*);

// When a Synth is created on the Server, the UGen constructors are only called during
// the first "next" routine, so if we send a unit command right after /s_new, the receiving
// unit hasn't been properly constructed. The previous version of VSTPlugin just ignored
// such unit commands and posted a warning, now we queue them and run them in the constructor.

// Unit commands likely trigger ansynchronous commands - which is not a problem in Scsynth.
// In Supernova there's a theoretical race condition issue since the system FIFO is single producer only,
// but UGen constructors never run in parallel, so this is safe as long as nobody else is scheduling
// system callbacks during the "next" routine (which would be dangerous anyway).

// In RT synthesis this is most useful for opening plugins right after Synth creation, e.g.:
// VSTPluginController(Synth(\test)).open("some_plugin", action: { |plugin| /* do something */ });

// In NRT synthesis this becomes even more useful because all commands are executed synchronously,
// so you can schedule /s_new + various unit commands (e.g. openMsg -> readProgramMsg) for the same timestamp.
template<VSTUnitCmdFunc fn>
void runUnitCmd(VSTPlugin* unit, sc_msg_iter* args) {
    if (unit->initialized()) {
        fn(unit, args); // the constructor has been called, so we can savely run the command
    } else {
        unit->queueUnitCmd((UnitCmdFunc)fn, args); // queue it
    }
}

#define UnitCmd(x) DefineUnitCmd("VSTPlugin", "/" #x, (UnitCmdFunc)runUnitCmd<vst_##x>)
#define PluginCmd(x) DefinePlugInCmd("/" #x, x, 0)

PluginLoad(VSTPlugin) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
    DefineDtorCantAliasUnit(VSTPlugin);
    UnitCmd(open);
    UnitCmd(close);
    UnitCmd(reset);
    UnitCmd(vis);
    UnitCmd(set);
    UnitCmd(setn);
    UnitCmd(param_query);
    UnitCmd(get);
    UnitCmd(getn);
    UnitCmd(map);
    UnitCmd(unmap);
    UnitCmd(program_set);
    UnitCmd(program_query);
    UnitCmd(program_name);
    UnitCmd(program_read);
    UnitCmd(program_write);
    UnitCmd(bank_read);
    UnitCmd(bank_write);
    UnitCmd(midi_msg);
    UnitCmd(midi_sysex);
    UnitCmd(tempo);
    UnitCmd(time_sig);
    UnitCmd(transport_play);
    UnitCmd(transport_set);
    UnitCmd(transport_get);
    UnitCmd(can_do);
    UnitCmd(vendor_method);

    PluginCmd(vst_search);
    PluginCmd(vst_clear);
    PluginCmd(vst_probe);
}


