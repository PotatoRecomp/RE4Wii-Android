// disc_import_jni.cpp - the launcher's way into disc_extract (com.shiinobu.re4.DiscImport).
#include "disc_extract.h"

#include <fcntl.h>
#include <jni.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

disc::ReadAt FileReader(int fd) {
    return [fd](uint64_t offset, void* out, size_t size) {
        auto* bytes = static_cast<char*>(out);
        while (size != 0) {
            const ssize_t got = pread64(fd, bytes, size, static_cast<off64_t>(offset));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return false;
            bytes += got;
            offset += static_cast<uint64_t>(got);
            size -= static_cast<size_t>(got);
        }
        return true;
    };
}

}  // namespace

extern "C" {

JNIEXPORT jbyteArray JNICALL Java_com_shiinobu_re4_DiscImport_readHeader(JNIEnv* env, jclass, jint fd) {
    uint8_t header[8];
    std::string message;
    if (!disc::ReadHeader(FileReader(fd), header, message)) {
        env->ThrowNew(env->FindClass("java/io/IOException"), message.c_str());
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(sizeof(header));
    if (result != nullptr) {
        env->SetByteArrayRegion(result, 0, sizeof(header), reinterpret_cast<const jbyte*>(header));
    }
    return result;
}

JNIEXPORT jstring JNICALL Java_com_shiinobu_re4_DiscImport_extract(JNIEnv* env, jclass, jint fd, jstring outDir,
                                                                       jlong freeBytes, jobject listener) {
    const char* chars = env->GetStringUTFChars(outDir, nullptr);
    const std::string path(chars);
    env->ReleaseStringUTFChars(outDir, chars);
    const jmethodID onProgress = env->GetMethodID(env->GetObjectClass(listener), "onProgress", "(JJ)Z");
    const disc::Result result = disc::Extract(FileReader(fd), path, static_cast<uint64_t>(freeBytes),
        [&](uint64_t done, uint64_t total) {
            return env->CallBooleanMethod(listener, onProgress, static_cast<jlong>(done), static_cast<jlong>(total)) == JNI_TRUE;
        });
    switch (result.status) {
        case disc::Result::Status::Ok: return nullptr;
        case disc::Result::Status::Cancelled: return env->NewStringUTF("");
        default: return env->NewStringUTF(result.message.c_str());
    }
}

JNIEXPORT jlongArray JNICALL Java_com_shiinobu_re4_DiscImport_fileSizes(JNIEnv* env, jclass, jint fd,
                                                                          jobjectArray paths) {
    std::string message;
    const std::unique_ptr<disc::GameDisc> disc = disc::GameDisc::Open(FileReader(fd), message);
    if (!disc) {
        env->ThrowNew(env->FindClass("java/io/IOException"), message.c_str());
        return nullptr;
    }
    const jsize count = env->GetArrayLength(paths);
    std::vector<jlong> sizes(static_cast<size_t>(count), -1);
    for (jsize i = 0; i < count; ++i) {
        auto* path = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        const char* chars = env->GetStringUTFChars(path, nullptr);
        for (const disc::DiscFile& file : disc->Files()) {
            if (file.path == chars) sizes[static_cast<size_t>(i)] = static_cast<jlong>(file.size);
        }
        env->ReleaseStringUTFChars(path, chars);
        env->DeleteLocalRef(path);
    }
    jlongArray result = env->NewLongArray(count);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, count, sizes.data());
    return result;
}

// NativeLib.prepare: the APK carries libmain.so zstd-compressed (assets/native/libmain.so.zst, stored
// uncompressed in the APK so it has an fd); this writes it out once per install. Null when done, else why not.
JNIEXPORT jstring JNICALL Java_com_shiinobu_re4_DiscImport_unpackZstd(JNIEnv* env, jclass, jint fd, jlong offset,
                                                                        jlong length, jstring outPath) {
    const char* chars = env->GetStringUTFChars(outPath, nullptr);
    const int out = open(chars, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    env->ReleaseStringUTFChars(outPath, chars);
    if (out < 0) return env->NewStringUTF(("cannot create the file: " + std::string(strerror(errno))).c_str());
    std::unique_ptr<ZSTD_DStream, size_t (*)(ZSTD_DStream*)> stream(ZSTD_createDStream(), ZSTD_freeDStream);
    std::vector<char> in(ZSTD_DStreamInSize() * 8), buf(ZSTD_DStreamOutSize() * 8);
    const disc::ReadAt read = FileReader(fd);
    std::string error;
    size_t last = 1;
    for (jlong done = 0; error.empty() && done < length;) {
        const size_t take = static_cast<size_t>(std::min<jlong>(static_cast<jlong>(in.size()), length - done));
        if (!read(static_cast<uint64_t>(offset + done), in.data(), take)) { error = "cannot read the APK"; break; }
        done += static_cast<jlong>(take);
        ZSTD_inBuffer src{in.data(), take, 0};
        while (error.empty() && src.pos < src.size) {
            ZSTD_outBuffer dst{buf.data(), buf.size(), 0};
            last = ZSTD_decompressStream(stream.get(), &dst, &src);
            if (ZSTD_isError(last)) { error = ZSTD_getErrorName(last); break; }
            for (size_t w = 0; w < dst.pos;) {
                const ssize_t n = write(out, buf.data() + w, dst.pos - w);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { error = "cannot write (" + std::string(strerror(errno)) + "): is the phone full?"; break; }
                w += static_cast<size_t>(n);
            }
        }
    }
    if (error.empty() && last != 0) error = "the packed library is cut short";
    if (close(out) != 0 && error.empty()) error = "cannot write: " + std::string(strerror(errno));
    return error.empty() ? nullptr : env->NewStringUTF(error.c_str());
}

}  // extern "C"
