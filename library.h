#ifndef LOGOS_BLOCKCHAIN_MODULE_API_H
#define LOGOS_BLOCKCHAIN_MODULE_API_H

#include <core/interface.h>

class LogosBlockchainModuleAPI : public QObject, public PluginInterface {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)

public:
    using QObject::QObject;
    ~LogosBlockchainModuleAPI() override = default;

    Q_INVOKABLE virtual int start(const QString& config_path) = 0;
    Q_INVOKABLE virtual void stop() = 0;
    Q_INVOKABLE virtual void initLogos(LogosAPI* logosAPIInstance) = 0;

    signals:
        void eventResponse(const QString& eventName, const QVariantList& data);
};

#define LogosBlockchainModuleInterface_iid "org.logos.blockchaininterface"
Q_DECLARE_INTERFACE(LogosBlockchainModuleAPI, LogosBlockchainModuleInterface_iid)

#endif
