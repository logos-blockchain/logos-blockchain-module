#include "library.h"

#include <QtCore/QObject>
#include <QtCore/QDebug>
#include <core/interface.h>

class LogosBlockchainModule : public QObject, public LogosBlockchainModuleAPI {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LogosBlockchainModuleInterface_iid FILE "metadata.json")
    Q_INTERFACES(LogosBlockchainModuleAPI PluginInterface)
public:
    LogosBlockchainModule() : node(nullptr) {
    }

    virtual ~LogosBlockchainModule() {
        if (node != nullptr) {
            stop();
        }
    }

    // PluginInterface implementation
    QString name() const override { return "logos-blockchain-module"; }
    QString version() const override { return "1.0.0"; }

    void initLogos(LogosAPI* logosAPIInstance) {
        logosAPI = logosAPIInstance;
        // logos = new LogosModules(logosAPI); // generated wrappers aggregator
        // logos->core_manager.setEventSource(this); // enable trigger() helper
    }

    Q_INVOKABLE void start(const QString &config_path) override {
        if (node != nullptr) {
            qWarning() << "Node already started";
            return;
        }

        QByteArray configPathBytes = config_path.toUtf8();
        InitializedNomosNodeResult result = start_nomos_node(configPathBytes.constData());

        if (result.error_code != None) {
            qCritical() << "Failed to start Nomos node. Error code:" << result.error_code;
            return;
        }

        node = result.nomos_node;
        qInfo() << "Nomos node started successfully";
    }

    Q_INVOKABLE void stop() override {
        if (node == nullptr) {
            qWarning() << "Node not running";
            return;
        }

        NomosNodeErrorCode error_code = stop_node(node);

        if (error_code != None) {
            qCritical() << "Failed to stop Nomos node. Error code:" << error_code;
        } else {
            qInfo() << "Nomos node stopped successfully";
        }

        node = nullptr;
    }

    signals:
    // Required for event forwarding between modules
    void eventResponse(const QString &eventName, const QVariantList &data) {
        
    }

private:
    NomosNode *node;
};
