/* BarmAPI.cpp: see BarmAPI.h. */

#include "config.h"
#include "BarmAPI.h"

#include "APICast.h"
#include "BytecodeCacheError.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "Completion.h"
#include "InitializeThreading.h"
#include "JSCInlines.h"
#include "Options.h"
#include "SourceProvider.h"
#include <unistd.h>
#include <wtf/FileHandle.h>
#include <wtf/RunLoop.h>
#include <wtf/text/StringImpl.h>

using namespace JSC;

namespace {

/* A script's text as the caller gave it (not copied when it's ASCII), and its bytecode if it has
 * some cached. */
class BarmSourceProvider final : public SourceProvider {
public:
    static Ref<BarmSourceProvider> create(const char* source, size_t len, const char* url)
    {
        return adoptRef(*new BarmSourceProvider(text(source, len), String::fromUTF8(url)));
    }

    unsigned hash() const final { return m_source.hash(); }
    StringView source() const final { return m_source; }
    RefPtr<CachedBytecode> cachedBytecode() const final { return m_cachedBytecode; }
    void setCachedBytecode(Ref<CachedBytecode>&& bytecode) { m_cachedBytecode = WTF::move(bytecode); }

private:
    BarmSourceProvider(String&& source, String&& url)
        : SourceProvider(SourceOrigin { URL({ }, url) }, String(url), String(), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::Program)
        , m_source(WTF::move(source))
    {
    }

    static String text(const char* source, size_t len)
    {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        std::span<const Latin1Character> bytes { reinterpret_cast<const Latin1Character*>(source), len };
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        if (charactersAreAllASCII(bytes))
            return StringImpl::createWithoutCopying(bytes);
        return String::fromUTF8(std::span<const char8_t> { reinterpret_cast<const char8_t*>(bytes.data()), bytes.size() });
    }

    String m_source;
    RefPtr<CachedBytecode> m_cachedBytecode;
};

}

JSValueRef BMEvaluateScript(JSContextRef ctx, const char* source, size_t len, const char* url, int cacheFd, bool* usedCache, JSValueRef* exception)
{
    JSGlobalObject* globalObject = toJS(ctx);
    VM& vm = globalObject->vm();
    JSLockHolder locker(vm);
    Ref provider = BarmSourceProvider::create(source, len, url);
    if (usedCache)
        *usedCache = false;
    if (cacheFd >= 0) {
        auto handle = FileSystem::FileHandle::adopt(dup(cacheFd));
        if (auto mapped = handle.map(FileSystem::MappedFileMode::Private)) {
            Ref bytecode = CachedBytecode::create(WTF::move(*mapped));
            SourceCodeKey key = sourceCodeKeyForSerializedProgram(vm, SourceCode(provider.copyRef()));
            if (isCachedBytecodeStillValid(vm, bytecode.copyRef(), key, SourceCodeType::ProgramType)) {
                provider->setCachedBytecode(WTF::move(bytecode));
                if (usedCache)
                    *usedCache = true;
            }
        }
    }
    NakedPtr<Exception> evaluationException;
    JSValue result = profiledEvaluate(globalObject, ProfilingReason::API, SourceCode(WTF::move(provider)), JSValue(), evaluationException);
    if (evaluationException) {
        if (exception)
            *exception = toRef(globalObject, evaluationException->value());
        return nullptr;
    }
    return toRef(globalObject, result ? result : jsUndefined());
}

bool BMWriteBytecode(JSContextGroupRef group, const char* source, size_t len, const char* url, int fd)
{
    VM& vm = *toJS(group);
    JSLockHolder locker(vm);
    auto handle = FileSystem::FileHandle::adopt(dup(fd));
    if (!handle)
        return false;
    BytecodeCacheError error;
    RefPtr written = generateProgramBytecode(vm, SourceCode(BarmSourceProvider::create(source, len, url)), handle, error);
    return written && !error.isValid();
}

double BMRunLoopSecondsUntilWork()
{
    Seconds until = RunLoop::currentSingleton().secondsUntilWork();
    return until.isInfinity() ? -1 : until.seconds();
}

void BMRunLoopCycle()
{
    RunLoop::cycle();
}

void BMRunLoopSetWakeUp(void (*wake)(void))
{
    RunLoop::setWakeUpCallback([wake] {
        wake();
    });
}

bool BMSetOptions(const char* options)
{
    JSC::initialize();
    return Options::setOptions(options);
}
