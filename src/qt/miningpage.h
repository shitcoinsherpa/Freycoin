// Copyright (c) 2014-2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_QT_MININGPAGE_H
#define FREYCOIN_QT_MININGPAGE_H

#include <pow/pow_common.h>

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <string>

class ClientModel;
class WalletModel;
class MiningEngine;
class PoWProcessor;

namespace Ui {
    class MiningPage;
}

namespace interfaces {
    class Node;
}

/**
 * GPU device information structure
 */
struct GPUDevice {
    int id;
    std::string name;
    size_t memory;
    int computeCapability;
    bool available;
};

/**
 * Mining page - provides GUI controls for CPU and GPU mining.
 *
 * Features:
 * - Automatic GPU/CUDA detection
 * - CPU thread count control
 * - Real-time statistics display
 * - Solo mining to wallet address
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */
class MiningPage : public QWidget
{
    Q_OBJECT

public:
    explicit MiningPage(QWidget *parent = nullptr);
    ~MiningPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);

public Q_SLOTS:
    /** Start mining with current settings */
    void startMining();
    /** Stop all mining */
    void stopMining();
    /** Refresh hardware detection */
    void refreshHardware();

Q_SIGNALS:
    void miningStarted();
    void miningStopped();
    void blockFound(int height, double merit);

private Q_SLOTS:
    void on_buttonStartMining_clicked();
    void on_buttonStopMining_clicked();
    void on_buttonRefreshHardware_clicked();
    void on_sliderCPUCores_valueChanged(int value);
    void on_groupBoxCPUMining_toggled(bool checked);
    void on_groupBoxGPUMining_toggled(bool checked);
    void on_sliderGPUIntensity_valueChanged(int value);

    void updateStats();

private:
    Ui::MiningPage *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;
    QTimer *statsTimer;
    QElapsedTimer miningTimer;

    bool m_isMining;
    bool m_cpuMiningEnabled;
    bool m_gpuMiningEnabled;
    int m_numThreads;
    int m_gpuIntensity; // 1-10, controls GPU batch size

    std::vector<GPUDevice> gpuDevices;
    MiningStatsSnapshot m_lastStats;

    uint64_t m_blocksFound;
    uint32_t m_bestGap;
    double m_bestMerit;

    // Cumulative stats (survive across per-block engine resets)
    uint64_t m_totalPrimes{0};
    uint64_t m_totalNonces{0};
    MiningStatsSnapshot m_prevStats{};

    // Mining engine (owned by mining thread)
    std::unique_ptr<MiningEngine> m_engine;
    std::atomic<bool> m_stopRequested{false};
    std::thread m_miningThread;

    // Hardware detection
    void detectCPU();
    void detectGPU();
    bool detectCUDA();
    void populateGPUComboBox();

    // Mining control
    void miningThreadFunc();
    QString getMiningAddress();

    // UI helpers
    void updateUIState();
    void logMessage(const QString &message);
    QString formatHashrate(double primesPerSec);
    QString formatUptime(qint64 seconds);

    // Settings persistence
    void loadSettings();
    void saveSettings();
};

#endif // FREYCOIN_QT_MININGPAGE_H
