#ifndef UNTITLED_LIBRARY_H
#define UNTITLED_LIBRARY_H

#include <core/interface.h>
#include <libnomos.h>

class LogosBlockchainModuleAPI: public PluginInterface {
private:
    NomosNode* node;
public:
    virtual ~LogosBlockchainModuleAPI() {}

    // Public API methods - must be Q_INVOKABLE for remote access
    Q_INVOKABLE virtual void start(const QString& config_path) = 0;
    Q_INVOKABLE virtual void stop() = 0;

    Q_INVOKABLE void initLogos(LogosAPI* logosAPIInstance);

    signals:
        // Required for event forwarding between modules
        void eventResponse(const QString &eventName, const QVariantList &data);
};
// Register interface with Qt's meta-object system
#define LogosBlockchainModuleInterface_iid "org.logos.blockchaininterface"
Q_DECLARE_INTERFACE(LogosBlockchainModuleAPI, LogosBlockchainModuleInterface_iid)
#endif // UNTITLED_LIBRARY_H