// Copyright (c) 2014-2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/miningpage.h>
#include <qt/forms/ui_miningpage.h>

#include <qt/clientmodel.h>
#include <qt/walletmodel.h>
#include <qt/guiutil.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <logging.h>
#include <outputtype.h>
#include <pow/mining_engine.h>
#include <pow/pow.h>
#include <pow/pow_processor.h>
#include <pow/fast_nextprime.h>
#include <gpu/opencl_loader.h>
#include <gpu/opencl_fermat.h>
#include <gpu/cuda_loader.h>
#include <gpu/cuda_fermat.h>

#include <interfaces/mining.h>
#include <node/context.h>
#include <addresstype.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <pow.h>
#include <validation.h>

#include <QSettings>
#include <QMessageBox>
#include <QDateTime>
#include <QProcess>
#include <QThread>

#include <algorithm>

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fstream>
#include <sstream>
#endif

// GPU detection via dynamic loading (no SDK needed at build time)

namespace {
/**
 * Captures valid prime gap proofs from MiningEngine.
 * Called from worker threads; stores results for the mining loop to process.
 */
class GuiMiningProcessor : public PoWProcessor {
public:
    CBlock* block;
    std::atomic<bool> found{false};
    uint32_t found_nonce{0};
    uint16_t found_shift{0};
    uint256 found_add;
    uint64_t found_gap{0};
    double found_merit{0.0};

    explicit GuiMiningProcessor(CBlock* b) : block(b) {}

    bool process(PoW* pow) override {
        found_nonce = pow->get_nonce();
        found_shift = pow->get_shift();

        std::vector<uint8_t> adder_bytes;
        pow->get_adder(adder_bytes);
        found_add.SetNull();
        if (!adder_bytes.empty()) {
            size_t copy_len = std::min(adder_bytes.size(), size_t(32));
            std::memcpy(found_add.begin(), adder_bytes.data(), copy_len);
        }

        found_gap = pow->gap_len();
        uint64_t merit_fp = pow->merit();
        found_merit = static_cast<double>(merit_fp) / (1ULL << 48);

        found = true;
        return false;  // Stop mining — solution accepted
    }
};
} // anonymous namespace

MiningPage::MiningPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::MiningPage),
    clientModel(nullptr),
    walletModel(nullptr),
    m_isMining(false),
    m_cpuMiningEnabled(false),
    m_gpuMiningEnabled(false),
    m_numThreads(1),
    m_gpuIntensity(5),
    m_blocksFound(0),
    m_bestGap(0),
    m_bestMerit(0.0)
{
    ui->setupUi(this);

    // Initialize stats
    m_lastStats = {};

    // Setup stats update timer
    statsTimer = new QTimer(this);
    connect(statsTimer, &QTimer::timeout, this, &MiningPage::updateStats);

    // Shutdown polling timer — checks if mining thread has exited
    m_shutdownTimer = new QTimer(this);
    m_shutdownTimer->setInterval(100);
    connect(m_shutdownTimer, &QTimer::timeout, this, &MiningPage::checkThreadFinished);

    // Connect slider to spinbox
    connect(ui->sliderCPUCores, &QSlider::valueChanged, ui->spinCPUCores, &QSpinBox::setValue);
    connect(ui->spinCPUCores, QOverload<int>::of(&QSpinBox::valueChanged), ui->sliderCPUCores, &QSlider::setValue);

    // Load saved settings
    loadSettings();

    // Detect hardware
    refreshHardware();

    // Initial UI state
    updateUIState();
}

MiningPage::~MiningPage()
{
    // Stop the shutdown polling timer
    if (m_shutdownTimer) {
        m_shutdownTimer->stop();
    }

    // Ensure mining stops and thread exits before destruction
    m_stopRequested = true;
    if (m_engine) {
        m_engine->stop();  // Blocking stop is OK in destructor
    }
    if (m_miningThread.joinable()) {
        m_miningThread.join();
    }
    saveSettings();
    delete ui;
}

void MiningPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
}

void MiningPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
}

void MiningPage::detectCPU()
{
    int cores = QThread::idealThreadCount();
    if (cores < 1) cores = 1;

    QString cpuInfo;

#ifdef WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    cores = sysInfo.dwNumberOfProcessors;
    cpuInfo = QString("%1 cores available").arg(cores);
#else
    // Try to get CPU model name on Linux
    std::ifstream cpufile("/proc/cpuinfo");
    std::string line;
    std::string modelName = "Unknown CPU";
    while (std::getline(cpufile, line)) {
        if (line.find("model name") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                modelName = line.substr(pos + 2);
                break;
            }
        }
    }
    cpuInfo = QString::fromStdString(modelName) + QString(" (%1 cores)").arg(cores);
#endif

    ui->labelCPUInfo->setText(cpuInfo);

    // Set slider maximum to detected cores
    ui->sliderCPUCores->setMaximum(cores);
    ui->spinCPUCores->setMaximum(cores);

    // Default to half the cores
    int defaultThreads = cores / 2;
    if (defaultThreads < 1) defaultThreads = 1;

    // Only set if not already loaded from settings
    if (ui->sliderCPUCores->value() == 0 || ui->sliderCPUCores->value() > cores) {
        ui->sliderCPUCores->setValue(defaultThreads);
        ui->spinCPUCores->setValue(defaultThreads);
    }

    logMessage(QString("Detected CPU: %1").arg(cpuInfo));
}

void MiningPage::detectGPU()
{
    gpuDevices.clear();
    int deviceIdx = 0;

    // Always detect GPUs at runtime — users may use pre-compiled builds
    // Compile flags only control whether GPU mining is actually usable

    // Runtime GPU detection via system tools (works without OpenCL SDK)
#ifdef WIN32
    // === NVIDIA Detection via nvidia-smi ===
    QProcess nvidiaSmi;
    nvidiaSmi.start("nvidia-smi", QStringList() << "--query-gpu=name,memory.total" << "--format=csv,noheader,nounits");
    if (nvidiaSmi.waitForFinished(5000)) {
        QString output = QString::fromUtf8(nvidiaSmi.readAllStandardOutput()).trimmed();
        if (!output.isEmpty()) {
            QStringList lines = output.split('\n');
            for (int i = 0; i < lines.size(); i++) {
                QString line = lines[i].trimmed();
                if (!line.isEmpty()) {
                    GPUDevice dev;
                    dev.id = deviceIdx++;
                    QStringList parts = line.split(',');
                    dev.name = parts[0].trimmed().toStdString();
                    if (parts.size() > 1) {
                        dev.memory = parts[1].trimmed().toLongLong() * 1024 * 1024;
                    }
                    dev.computeCapability = 0;
                    dev.available = true;
                    gpuDevices.push_back(dev);
                    logMessage(QString("Found NVIDIA GPU: %1").arg(QString::fromStdString(dev.name)));
                }
            }
        }
    }

    // === AMD Detection via WMI (Windows) ===
    QProcess wmic;
    wmic.start("wmic", QStringList() << "path" << "win32_VideoController" << "get" << "Name,AdapterRAM" << "/format:csv");
    if (wmic.waitForFinished(5000)) {
        QString output = QString::fromUtf8(wmic.readAllStandardOutput());
        QStringList lines = output.split('\n');
        for (const QString& line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith("Node")) continue;

            QStringList parts = trimmed.split(',');
            if (parts.size() >= 3) {
                QString adapterRam = parts[1].trimmed();
                QString name = parts[2].trimmed();

                if (name.contains("AMD", Qt::CaseInsensitive) ||
                    name.contains("Radeon", Qt::CaseInsensitive)) {
                    GPUDevice dev;
                    dev.id = deviceIdx++;
                    dev.name = name.toStdString();
                    dev.memory = adapterRam.toLongLong();
                    dev.computeCapability = 0;
                    dev.available = true;
                    gpuDevices.push_back(dev);
                    logMessage(QString("Found AMD GPU: %1").arg(name));
                }
            }
        }
    }

#else
    // === Linux: NVIDIA via nvidia-smi ===
    FILE *fp = popen("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    if (fp) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), fp)) {
            QString line = QString::fromUtf8(buffer).trimmed();
            if (!line.isEmpty()) {
                GPUDevice dev;
                dev.id = deviceIdx++;
                QStringList parts = line.split(',');
                dev.name = parts[0].trimmed().toStdString();
                if (parts.size() > 1) {
                    dev.memory = parts[1].trimmed().toLongLong() * 1024 * 1024;
                }
                dev.computeCapability = 0;
                dev.available = true;
                gpuDevices.push_back(dev);
                logMessage(QString("Found NVIDIA GPU: %1").arg(QString::fromStdString(dev.name)));
            }
        }
        pclose(fp);
    }

    // === Linux: AMD via rocm-smi or /sys/class/drm ===
    fp = popen("rocm-smi --showproductname --csv 2>/dev/null | tail -n +2", "r");
    if (fp) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), fp)) {
            QString line = QString::fromUtf8(buffer).trimmed();
            if (!line.isEmpty() && !line.contains("device")) {
                QStringList parts = line.split(',');
                if (parts.size() >= 2) {
                    GPUDevice dev;
                    dev.id = deviceIdx++;
                    dev.name = parts[1].trimmed().toStdString();
                    dev.memory = 0;
                    dev.computeCapability = 0;
                    dev.available = true;
                    gpuDevices.push_back(dev);
                    logMessage(QString("Found AMD GPU: %1").arg(QString::fromStdString(dev.name)));
                }
            }
        }
        pclose(fp);
    }

    // Fallback: /sys/class/drm for AMD cards
    if (gpuDevices.empty() || std::none_of(gpuDevices.begin(), gpuDevices.end(),
            [](const GPUDevice& d) { return d.name.find("AMD") != std::string::npos ||
                                           d.name.find("Radeon") != std::string::npos; })) {
        fp = popen("ls -d /sys/class/drm/card*/device/vendor 2>/dev/null | xargs cat 2>/dev/null", "r");
        if (fp) {
            char buffer[64];
            int cardIdx = 0;
            while (fgets(buffer, sizeof(buffer), fp)) {
                QString vendor = QString::fromUtf8(buffer).trimmed();
                if (vendor == "0x1002") {
                    QString cardPath = QString("/sys/class/drm/card%1/device").arg(cardIdx);
                    std::ifstream nameFile((cardPath + "/product_name").toStdString());
                    std::string cardName = "AMD Radeon GPU";
                    if (nameFile.is_open()) {
                        std::getline(nameFile, cardName);
                    }
                    GPUDevice dev;
                    dev.id = deviceIdx++;
                    dev.name = cardName;
                    dev.memory = 0;
                    dev.computeCapability = 0;
                    dev.available = true;
                    gpuDevices.push_back(dev);
                    logMessage(QString("Found AMD GPU: %1").arg(QString::fromStdString(dev.name)));
                }
                cardIdx++;
            }
            pclose(fp);
        }
    }
#endif

    if (!gpuDevices.empty()) {
        populateGPUComboBox();

        // Determine GPU mining capability at runtime
        bool gpuMiningAvailable = false;
        QString backendInfo;

        // Try CUDA first (preferred for NVIDIA — lower overhead)
        if (cuda_load() == 0) {
            int cudaDevices = cuda_get_device_count();
            if (cudaDevices > 0) {
                gpuMiningAvailable = true;
                const char* name = cuda_get_device_name(0);
                backendInfo = QString("CUDA (%1 device%2 — %3)")
                    .arg(cudaDevices).arg(cudaDevices > 1 ? "s" : "")
                    .arg(name ? QString::fromUtf8(name) : "NVIDIA");
            }
        }

        // Fallback to OpenCL (works with any GPU vendor)
        if (!gpuMiningAvailable && opencl_load() == 0) {
            int oclDevices = opencl_get_device_count();
            if (oclDevices > 0) {
                gpuMiningAvailable = true;
                bool hasNvidia = false, hasAmd = false;
                for (const auto& dev : gpuDevices) {
                    if (dev.name.find("NVIDIA") != std::string::npos ||
                        dev.name.find("GeForce") != std::string::npos ||
                        dev.name.find("RTX") != std::string::npos)
                        hasNvidia = true;
                    if (dev.name.find("AMD") != std::string::npos ||
                        dev.name.find("Radeon") != std::string::npos)
                        hasAmd = true;
                }
                QString vendor;
                if (hasNvidia && hasAmd) vendor = "NVIDIA+AMD";
                else if (hasNvidia) vendor = "NVIDIA";
                else if (hasAmd) vendor = "AMD";
                else vendor = "";

                QString vendorLabel = vendor.isEmpty() ? QString() : (QString(" ") + vendor);
                backendInfo = QString("OpenCL%1 (%2 device%3)")
                    .arg(vendorLabel)
                    .arg(oclDevices).arg(oclDevices > 1 ? "s" : "");
            }
        }

        if (!gpuMiningAvailable) {
            backendInfo = "GPU detected, install GPU drivers for mining";
        }

        ui->groupBoxGPUMining->setEnabled(gpuMiningAvailable);
        ui->labelGPUInfo->setText(QString("%1 GPU(s) — %2").arg(gpuDevices.size()).arg(backendInfo));
        logMessage(QString("Total %1 GPU(s) detected. Mining backend: %2")
            .arg(gpuDevices.size()).arg(backendInfo));
        return;
    }

    // No GPUs found
    ui->groupBoxGPUMining->setEnabled(false);
    ui->groupBoxGPUMining->setChecked(false);
    ui->labelGPUInfo->setText("No GPU detected");
    logMessage("No GPU detected for mining");
}

void MiningPage::populateGPUComboBox()
{
    ui->comboGPUDevice->clear();

    for (size_t i = 0; i < gpuDevices.size(); i++) {
        QString label = QString("GPU %1: %2")
            .arg(gpuDevices[i].id)
            .arg(QString::fromStdString(gpuDevices[i].name));

        if (gpuDevices[i].memory > 0) {
            label += QString(" (%1 GB)").arg(gpuDevices[i].memory / (1024.0*1024.0*1024.0), 0, 'f', 1);
        }

        ui->comboGPUDevice->addItem(label, gpuDevices[i].id);
    }

    if (gpuDevices.empty()) {
        ui->comboGPUDevice->addItem("No GPU available");
        ui->comboGPUDevice->setEnabled(false);
    }
}

void MiningPage::refreshHardware()
{
    logMessage("Detecting hardware...");
    detectCPU();
    detectGPU();
    logMessage("Hardware detection complete");
}

QString MiningPage::getMiningAddress()
{
    if (!walletModel) {
        return QString();
    }

    // Get a new receiving address from the wallet for mining rewards
    // Try BECH32M first
    if (auto op_dest = walletModel->wallet().getNewDestination(OutputType::BECH32M, "Mining")) {
        return QString::fromStdString(EncodeDestination(*op_dest));
    }

    // Fallback to BECH32
    if (auto op_dest = walletModel->wallet().getNewDestination(OutputType::BECH32, "Mining")) {
        return QString::fromStdString(EncodeDestination(*op_dest));
    }

    return QString();
}

void MiningPage::miningThreadFunc()
{
    auto cleanup = [this]() {
        QMetaObject::invokeMethod(this, [this]() { stopMining(); }, Qt::QueuedConnection);
    };

    if (!clientModel || !walletModel) {
        QMetaObject::invokeMethod(this, [this]() {
            logMessage("Error: Models not initialized");
        }, Qt::QueuedConnection);
        cleanup();
        return;
    }

    // Access the node context for block templates and block submission
    node::NodeContext* ctx = clientModel->node().context();
    if (!ctx || !ctx->chainman) {
        QMetaObject::invokeMethod(this, [this]() {
            logMessage("Error: Node not fully initialized");
        }, Qt::QueuedConnection);
        cleanup();
        return;
    }

    // Create Mining interface for block templates
    auto mining = interfaces::MakeMining(*ctx);

    // Get mining address and convert to coinbase output script
    QString miningAddress = getMiningAddress();
    if (miningAddress.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            logMessage("Error: Could not get mining address from wallet");
        }, Qt::QueuedConnection);
        cleanup();
        return;
    }

    CTxDestination dest = DecodeDestination(miningAddress.toStdString());
    if (!IsValidDestination(dest)) {
        QMetaObject::invokeMethod(this, [this]() {
            logMessage("Error: Invalid mining address");
        }, Qt::QueuedConnection);
        cleanup();
        return;
    }
    CScript coinbase_script = GetScriptForDestination(dest);

    QMetaObject::invokeMethod(this, [this, miningAddress]() {
        logMessage(QString("Mining to: %1").arg(miningAddress));
    }, Qt::QueuedConnection);

    // Create mining engine — auto-detects best GPU tier (CUDA > OpenCL > CPU)
    // If user disabled GPU mining, force CPU-only
    MiningTier tier = MiningTier::CPU_ONLY;
    if (m_gpuMiningEnabled && !gpuDevices.empty()) {
        // Let the engine detect the best available backend
        MiningEngine probe;
        tier = probe.get_tier();
    }

    m_engine = std::make_unique<MiningEngine>(tier, m_numThreads);
    m_engine->set_gpu_intensity(m_gpuIntensity);

    // Warm up gwnum's global state (shareable sincos mutex) with a single-
    // threaded call before workers launch.  gwnum's share_sincos_data() has a
    // racy mutex init — multiple threads calling gwsetup simultaneously on
    // first use double-initialize the mutex, corrupting the heap.
    // The dummy must be >= 2000 bits to hit the gwnum code path (below that
    // fast_nextprime falls back to GMP's mpz_nextprime and never touches gwnum).
    {
        mpz_t dummy_n, dummy_r;
        mpz_init(dummy_n);
        mpz_ui_pow_ui(dummy_n, 2, 2048);  // 2^2048 — a 2049-bit number
        mpz_nextprime(dummy_n, dummy_n);   // find an actual prime near 2^2048
        mpz_init(dummy_r);
        fast_nextprime(dummy_r, dummy_n);  // THIS now triggers gwnum init
        mpz_clear(dummy_r);
        mpz_clear(dummy_n);
    }

    QMetaObject::invokeMethod(this, [this]() {
        logMessage(QString("Mining engine: %1 (%2 threads, intensity %3)")
            .arg(m_engine->get_hardware_info())
            .arg(m_numThreads)
            .arg(m_gpuIntensity));
    }, Qt::QueuedConnection);

    // Mining loop — create block templates and mine them
    while (!m_stopRequested.load()) {
        // Wait until the node is fully caught up before mining.
        // Post-fork block validation calls fast_nextprime on 12K-bit numbers,
        // holding cs_main for ~9s (GPU lib), ~31s (gwnum), or ~78s (GMP).
        // If the node is syncing blocks from peers, createNewBlock() would
        // block indefinitely. Use TRY_LOCK to stay responsive and check both
        // cs_main availability AND chain tip recency (tip within 5 minutes).
        {
            bool notified_user = false;
            while (!m_stopRequested.load()) {
                // Wait for any previous async submit to finish.
                if (m_submitThread.joinable()) {
                    if (m_submitResult && !m_submitResult->done.load()) {
                        if (!notified_user) {
                            LogPrintf("Mining: Submitting block to node...\n");
                            QMetaObject::invokeMethod(this, [this]() {
                                logMessage("Submitting block to network...");
                            }, Qt::QueuedConnection);
                            notified_user = true;
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        continue;
                    }
                    m_submitThread.join();
                    // Process submit result (safe `this` access — we're on the mining thread)
                    if (m_submitResult) {
                        if (m_submitResult->accepted) {
                            m_blocksFound++;
                            uint64_t gap = m_submitResult->gap;
                            double merit = m_submitResult->merit;
                            if (static_cast<uint32_t>(gap) > m_bestGap) {
                                m_bestGap = static_cast<uint32_t>(gap);
                                m_bestMerit = merit;
                            }
                            LogPrintf("Mining: BLOCK ACCEPTED! Gap=%llu Merit=%.4f Total=%llu\n",
                                      gap, merit, m_blocksFound);
                            QMetaObject::invokeMethod(this, [this, gap, merit]() {
                                logMessage(QString("BLOCK ACCEPTED! Gap=%1 Merit=%2 Total: %3")
                                    .arg(gap).arg(merit, 0, 'f', 4).arg(m_blocksFound));
                            }, Qt::QueuedConnection);
                        } else if (m_submitResult->stale) {
                            LogPrintf("Mining: Block was stale\n");
                            QMetaObject::invokeMethod(this, [this]() {
                                logMessage("Block was stale — another miner found one first");
                            }, Qt::QueuedConnection);
                        } else {
                            LogPrintf("Mining: Block was rejected\n");
                            QMetaObject::invokeMethod(this, [this]() {
                                logMessage("Block rejected by node");
                            }, Qt::QueuedConnection);
                        }
                        m_submitResult.reset();
                    }
                    notified_user = false;
                }

                TRY_LOCK(cs_main, locked);
                if (locked) {
                    // Check if we've validated all known headers from peers.
                    // Do NOT gate on IsInitialBlockDownload() — on a young/low-hashrate
                    // chain the tip can be >24h old simply because no one has mined.
                    // Mining is what produces new blocks, so blocking it on tip age
                    // creates a deadlock. The header comparison is the correct check.
                    int chainHeight = ctx->chainman->ActiveChain().Height();
                    const CBlockIndex* bestHeader = ctx->chainman->m_best_header;
                    int headerHeight = bestHeader ? bestHeader->nHeight : chainHeight;
                    if (chainHeight >= headerHeight) {
                        // All known blocks validated — safe to mine
                        break;
                    }
                    // Show sync progress so the user sees block count ticking up.
                    int blocksLeft = headerHeight - chainHeight;
                    int estMinutes = (blocksLeft * 31) / 60;
                    LogPrintf("Mining: Chain at height %d, peers at %d (%d blocks behind), waiting...\n",
                              chainHeight, headerHeight, blocksLeft);
                    QMetaObject::invokeMethod(this, [this, chainHeight, headerHeight, blocksLeft, estMinutes]() {
                        if (estMinutes > 0) {
                            logMessage(QString("Syncing blocks: %1 / %2 (%3 remaining, ~%4 min)...")
                                .arg(chainHeight).arg(headerHeight).arg(blocksLeft).arg(estMinutes));
                        } else {
                            logMessage(QString("Syncing blocks: %1 / %2 (%3 remaining)...")
                                .arg(chainHeight).arg(headerHeight).arg(blocksLeft));
                        }
                    }, Qt::QueuedConnection);
                    notified_user = true;
                } else {
                    if (!notified_user) {
                        LogPrintf("Mining: cs_main locked (block validation in progress), waiting...\n");
                        QMetaObject::invokeMethod(this, [this]() {
                            logMessage("Validating incoming block (mining paused)...");
                        }, Qt::QueuedConnection);
                        notified_user = true;
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            if (m_stopRequested.load()) break;
            if (notified_user) {
                LogPrintf("Mining: Node ready, proceeding to create block template\n");
                QMetaObject::invokeMethod(this, [this]() {
                    logMessage("Blockchain synced. Preparing block template...");
                }, Qt::QueuedConnection);
            }
        }

        if (m_stopRequested.load()) break;

        try {
            // Create a new block template from the current chain tip
            auto tmpl = mining->createNewBlock({.coinbase_output_script = coinbase_script});
            if (!tmpl) {
                if (m_stopRequested.load()) break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            CBlock block = tmpl->getBlock();
            block.hashMerkleRoot = BlockMerkleRoot(block);

            // Set PoW parameters — shift computed from intensity, fork-aware
            uint16_t forkMinShift = MIN_SHIFT;
            {
                LOCK(cs_main);
                const CBlockIndex* pindexPrev = ctx->chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock);
                if (pindexPrev) {
                    forkMinShift = ctx->chainman->GetConsensus().GetMinShift(pindexPrev->nHeight + 1);
                }
            }
            const uint16_t shift = MiningEngine::compute_shift(m_gpuIntensity, forkMinShift);
            block.nShift = shift;
            block.nAdd.SetNull();
            block.nReserved = 0;

            // Build 84-byte header template for the mining engine
            std::vector<uint8_t> header_template;
            header_template.reserve(84);

            uint32_t version = static_cast<uint32_t>(block.nVersion);
            header_template.insert(header_template.end(),
                reinterpret_cast<uint8_t*>(&version),
                reinterpret_cast<uint8_t*>(&version) + 4);

            header_template.insert(header_template.end(),
                block.hashPrevBlock.begin(), block.hashPrevBlock.end());

            header_template.insert(header_template.end(),
                block.hashMerkleRoot.begin(), block.hashMerkleRoot.end());

            uint32_t time_val = block.nTime;
            header_template.insert(header_template.end(),
                reinterpret_cast<uint8_t*>(&time_val),
                reinterpret_cast<uint8_t*>(&time_val) + 4);

            uint64_t difficulty = block.nDifficulty;
            header_template.insert(header_template.end(),
                reinterpret_cast<uint8_t*>(&difficulty),
                reinterpret_cast<uint8_t*>(&difficulty) + 8);

            uint32_t nonce_placeholder = 0;
            header_template.insert(header_template.end(),
                reinterpret_cast<uint8_t*>(&nonce_placeholder),
                reinterpret_cast<uint8_t*>(&nonce_placeholder) + 4);

            constexpr size_t NONCE_OFFSET = 4 + 32 + 32 + 4 + 8; // 80

            // Create processor to capture valid proofs
            GuiMiningProcessor processor(&block);

            QMetaObject::invokeMethod(this, [this, difficulty, shift]() {
                logMessage(QString("Mining block at difficulty=%1 shift=%2")
                    .arg(difficulty, 16, 16, QChar('0'))
                    .arg(shift));
            }, Qt::QueuedConnection);

            // Mine — blocks until solution found or stop() called
            m_engine->mine_parallel(
                header_template,
                NONCE_OFFSET,
                shift,
                block.nDifficulty,
                0,  // start_nonce
                &processor
            );

            if (m_stopRequested.load()) break;

            // Process the found solution — submit asynchronously to avoid cs_main contention
            if (processor.found) {
                block.nNonce = processor.found_nonce;
                block.nShift = processor.found_shift;
                block.nAdd = processor.found_add;

                uint64_t found_gap = processor.found_gap;
                double found_merit = processor.found_merit;
                auto block_copy = std::make_shared<CBlock>(block);

                auto result = std::make_shared<SubmitResult>();
                m_submitResult = result;
                m_submitThread = std::thread([block_copy, ctx, result, found_gap, found_merit]() {
                    // No `this` capture — safe even if MiningPage is destroyed during submit
                    int nBlockHeight = -1;
                    {
                        LOCK(cs_main);
                        const CBlockIndex* pindexPrev = ctx->chainman->m_blockman.LookupBlockIndex(block_copy->hashPrevBlock);
                        if (pindexPrev) nBlockHeight = pindexPrev->nHeight + 1;
                        const CBlockIndex* tip = ctx->chainman->ActiveChain().Tip();
                        if (tip && tip->GetBlockHash() != block_copy->hashPrevBlock) {
                            LogPrintf("Mining: Block stale (height %d, tip now %d) — discarded\n",
                                      nBlockHeight, tip->nHeight);
                            result->stale = true;
                            result->done.store(true);
                            return;
                        }
                    }
                    // Mark block as pre-checked so ProcessNewBlock skips the expensive
                    // CheckProofOfWork (fast_nextprime: ~9s GPU, ~31s gwnum, ~78s GMP)
                    // under cs_main. The mining engine already verified the gap meets
                    // difficulty. Without this, cs_main is held for the full duration,
                    // blocking all RPC and UI updates.
                    block_copy->fChecked = true;
                    auto block_ptr = std::make_shared<const CBlock>(*block_copy);
                    LogPrintf("Mining: Submitting block height %d via ProcessNewBlock...\n", nBlockHeight);
                    if (ctx->chainman->ProcessNewBlock(block_ptr, /*force_processing=*/true, nullptr)) {
                        result->accepted = true;
                        result->gap = found_gap;
                        result->merit = found_merit;
                        LogPrintf("Mining: BLOCK ACCEPTED! Height=%d Gap=%llu Merit=%.4f\n",
                                  nBlockHeight, found_gap, found_merit);
                    } else {
                        LogPrintf("Mining: Block REJECTED by ProcessNewBlock (height %d)\n", nBlockHeight);
                    }
                    result->done.store(true);
                });
            }

            // Update stats
            if (m_engine) {
                m_lastStats = m_engine->get_stats();
            }

        } catch (const std::exception& e) {
            QMetaObject::invokeMethod(this, [this, msg = std::string(e.what())]() {
                logMessage(QString("Mining error: %1").arg(QString::fromStdString(msg)));
            }, Qt::QueuedConnection);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    // Clean up submit thread — give ProcessNewBlock time to finish before detaching
    if (m_submitThread.joinable()) {
        if (m_submitResult && !m_submitResult->done.load()) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!m_submitResult->done.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (m_submitResult && !m_submitResult->done.load()) {
            LogPrintf("Mining: Detaching submit thread (still validating after 5s timeout)\n");
            m_submitThread.detach();
        } else {
            m_submitThread.join();
        }
    }

    m_engine.reset();
    m_threadFinished = true;
}

void MiningPage::startMining()
{
    if (m_isMining) {
        logMessage("Mining already in progress");
        return;
    }

    m_cpuMiningEnabled = ui->groupBoxCPUMining->isChecked();
    m_gpuMiningEnabled = ui->groupBoxGPUMining->isChecked();

    if (!m_cpuMiningEnabled && !m_gpuMiningEnabled) {
        QMessageBox::warning(this, tr("Mining"), tr("Please enable CPU or GPU mining first."));
        return;
    }

    if (!walletModel) {
        QMessageBox::warning(this, tr("Mining"), tr("Wallet not loaded. Please wait for wallet to initialize."));
        return;
    }

    // Get thread count
    m_numThreads = ui->spinCPUCores->value();

    // Reset stats
    m_lastStats = {};
    m_prevStats = {};
    m_totalPrimes = 0;
    m_totalNonces = 0;
    m_blocksFound = 0;
    m_bestGap = 0;
    m_bestMerit = 0.0;

    logMessage(QString("Starting mining with %1 thread(s)...").arg(m_numThreads));

    // Previous thread should already be joined by checkThreadFinished()
    if (m_miningThread.joinable()) {
        logMessage("Previous mining session still stopping, please wait");
        return;
    }

    m_isMining = true;
    m_stopRequested = false;
    m_threadFinished = false;
    miningTimer.start();
    statsTimer->start(1000); // Update every second

    // Start mining thread
    m_miningThread = std::thread(&MiningPage::miningThreadFunc, this);

    updateUIState();
    Q_EMIT miningStarted();
    logMessage("Mining started");
}

void MiningPage::stopMining()
{
    if (!m_isMining) {
        return;
    }

    logMessage("Stopping mining...");

    m_isMining = false;
    m_stopRequested = true;

    // Signal engine workers to stop (non-blocking — just sets flags)
    if (m_engine) {
        m_engine->request_stop();
    }

    statsTimer->stop();

    // Disable both buttons while the mining thread winds down
    ui->buttonStartMining->setEnabled(false);
    ui->buttonStopMining->setEnabled(false);

    // Poll until the mining thread has exited, then join it
    m_shutdownTimer->start();

    Q_EMIT miningStopped();
}

void MiningPage::checkThreadFinished()
{
    if (!m_threadFinished.load()) return;

    m_shutdownTimer->stop();

    if (m_miningThread.joinable()) {
        m_miningThread.join(); // instant — thread already exited
    }
    m_threadFinished = false;

    logMessage("Mining stopped");
    updateUIState();
}

void MiningPage::updateStats()
{
    if (!m_isMining) {
        return;
    }

    // Update uptime
    qint64 uptimeSeconds = miningTimer.elapsed() / 1000;
    ui->labelUptime->setText(formatUptime(uptimeSeconds));

    // Get stats from engine
    if (m_engine) {
        m_lastStats = m_engine->get_stats();
    }

    // Detect engine stats reset (happens when a new block template starts)
    // and accumulate previous values into running totals
    if (m_lastStats.primes_found < m_prevStats.primes_found) {
        m_totalPrimes += m_prevStats.primes_found;
        m_totalNonces += m_prevStats.nonces_tested;
    }
    m_prevStats = m_lastStats;

    uint64_t cumulativePrimes = m_totalPrimes + m_lastStats.primes_found;
    uint64_t cumulativeNonces = m_totalNonces + m_lastStats.nonces_tested;

    // Calculate primes per second
    double primesPerSec = 0.0;
    if (uptimeSeconds > 0) {
        primesPerSec = static_cast<double>(cumulativePrimes) / uptimeSeconds;
    }

    ui->labelHashrate->setText(formatHashrate(primesPerSec));
    ui->labelPrimesFound->setText(QString::number(cumulativePrimes));
    ui->labelNoncesTested->setText(QString::number(cumulativeNonces));
    ui->labelBlocksFound->setText(QString::number(m_blocksFound));

    if (m_bestGap > 0) {
        ui->labelBestGap->setText(QString("%1 (merit: %2)")
            .arg(m_bestGap)
            .arg(m_bestMerit, 0, 'f', 4));
    }
}

void MiningPage::updateUIState()
{
    bool canMine = !m_isMining;
    bool miningActive = m_isMining;

    ui->buttonStartMining->setEnabled(canMine);
    ui->buttonStopMining->setEnabled(miningActive);

    ui->groupBoxCPUMining->setEnabled(canMine);
    ui->groupBoxGPUMining->setEnabled(canMine && !gpuDevices.empty());
    ui->buttonRefreshHardware->setEnabled(canMine);
}

void MiningPage::logMessage(const QString &message)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString logLine = QString("[%1] %2").arg(timestamp, message);
    ui->textEditLog->append(logLine);

    // Keep log from getting too large
    QTextCursor cursor = ui->textEditLog->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui->textEditLog->setTextCursor(cursor);

    // Also log to debug.log
    LogPrintf("MiningPage: %s\n", message.toStdString());
}

QString MiningPage::formatHashrate(double primesPerSec)
{
    if (primesPerSec < 1.0) {
        return QString("%1 primes/s").arg(primesPerSec, 0, 'f', 3);
    } else if (primesPerSec < 1000.0) {
        return QString("%1 primes/s").arg(primesPerSec, 0, 'f', 1);
    } else if (primesPerSec < 1000000.0) {
        return QString("%1 Kprimes/s").arg(primesPerSec / 1000.0, 0, 'f', 2);
    } else {
        return QString("%1 Mprimes/s").arg(primesPerSec / 1000000.0, 0, 'f', 2);
    }
}

QString MiningPage::formatUptime(qint64 seconds)
{
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(mins, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

void MiningPage::loadSettings()
{
    QSettings settings;

    int threads = settings.value("mining/cpuThreads", 0).toInt();
    if (threads > 0) {
        ui->spinCPUCores->setValue(threads);
        ui->sliderCPUCores->setValue(threads);
    }

    ui->groupBoxCPUMining->setChecked(settings.value("mining/cpuEnabled", false).toBool());
    ui->groupBoxGPUMining->setChecked(settings.value("mining/gpuEnabled", false).toBool());

    int intensity = settings.value("mining/gpuIntensity", 5).toInt();
    ui->sliderGPUIntensity->setValue(intensity);
    on_sliderGPUIntensity_valueChanged(intensity);
}

void MiningPage::saveSettings()
{
    QSettings settings;

    settings.setValue("mining/cpuThreads", ui->spinCPUCores->value());
    settings.setValue("mining/cpuEnabled", ui->groupBoxCPUMining->isChecked());
    settings.setValue("mining/gpuEnabled", ui->groupBoxGPUMining->isChecked());
    settings.setValue("mining/gpuIntensity", ui->sliderGPUIntensity->value());
}

// Slot implementations
void MiningPage::on_buttonStartMining_clicked()
{
    startMining();
}

void MiningPage::on_buttonStopMining_clicked()
{
    stopMining();
}

void MiningPage::on_buttonRefreshHardware_clicked()
{
    refreshHardware();
}

void MiningPage::on_sliderCPUCores_valueChanged(int value)
{
    Q_UNUSED(value);
    // Handled by signal connection to spinbox
}

void MiningPage::on_groupBoxCPUMining_toggled(bool checked)
{
    m_cpuMiningEnabled = checked;
}

void MiningPage::on_groupBoxGPUMining_toggled(bool checked)
{
    m_gpuMiningEnabled = checked;
}

void MiningPage::on_sliderGPUIntensity_valueChanged(int value)
{
    m_gpuIntensity = value;
    static const char* labels[] = {
        "1 (Minimal)", "2 (Very Low)", "3 (Low)", "4 (Below Medium)",
        "5 (Medium)", "6 (Above Medium)", "7 (High)", "8 (Very High)",
        "9 (Extreme)", "10 (Maximum)"
    };
    int idx = std::clamp(value, 1, 10) - 1;
    ui->labelGPUIntensityValue->setText(labels[idx]);
}
