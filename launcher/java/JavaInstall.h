#pragma once

#include "BaseVersion.h"
#include "JavaVersion.h"
#include <sys.h>

struct JavaInstall : public BaseVersion
{
    JavaInstall(){}
    JavaInstall(const QString& id, const Sys::Architecture& arch, const QString& path)
    : id(id), arch(arch), path(path)
    {
    }
    virtual QString descriptor() const
    {
        return id.toString();
    }

    virtual QString name() const
    {
        return id.toString();
    }

    virtual QString typeString() const
    {
        return arch.serialize();
    }

    bool operator<(const JavaInstall & rhs) const;
    bool operator==(const JavaInstall & rhs) const;
    bool operator>(const JavaInstall & rhs) const;

    JavaVersion id;
    Sys::Architecture arch;
    QString path;
    bool recommended = false;

private:
    using BaseVersion::operator<;
    using BaseVersion::operator>;
};

typedef std::shared_ptr<JavaInstall> JavaInstallPtr;
