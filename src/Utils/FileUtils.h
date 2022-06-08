//
// Created by Shenyrion on 2022/6/8.
//

#ifndef DEVIDROID_FILEUTILS_H
#define DEVIDROID_FILEUTILS_H

#include <string>

class FileUtils {
public:
    static std::shared_ptr <FileUtils> instance();

    std::string GetBinFile2String(const std::string &filename);

    std::string getStrFile2string(const std::string &filename);

    std::string getVariable(const std::string &url, const std::string &key);

private:
    FileUtils() = default;

    ~FileUtils() = default;

    FileUtils(const FileUtils &) = default;

    FileUtils &operator=(const FileUtils &);
};

#endif //DEVIDROID_FILEUTILS_H
