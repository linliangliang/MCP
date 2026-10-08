// Qt 界面客户端：
//   输入框用自然语言提问 → DeepSeek 决定要不要调 get_weather →
//   通过 MCP 客户端真的去问 weather-mcp → 结果交回 DeepSeek 组织成自然语言 →
//   追加显示在输出框（下一轮接着往下加）。
//
// 网络/子进程调用跑在 QtConcurrent 的线程池里，不占 UI 线程——
// 一次问答要两次大模型 + 一次天气 API，几秒钟的等待足以让窗口"未响应"。
//
// 注意 MainWindow 不能放进匿名命名空间：moc 生成的 staticMetaObject 定义
// 不带命名空间限定，放进去会链接不上。

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QFont>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include "mcp/agent.hpp"
#include "mcp/config.hpp"
#include "mcp/console.hpp"
#include "mcp/process.hpp"

namespace {

/// 命令行里不带问题时跑这两个内置问句：
/// 中文从命令行传进来容易被终端编码搅乱，写死在源码里（UTF-8）最稳。
/// 第二句故意不点城市名，用来验证对话历史真的接得上。
const std::vector<std::string> kDemoQuestions = {"武汉明天天气怎么样", "那上海呢"};

int runCli(const std::string& serverPath,
           const mcp::LlmConfig& llm,
           const std::vector<std::string>& questions) {
    mcp::WeatherAgent agent(serverPath, llm);
    agent.connect();
    for (const std::string& question : questions) {
        std::cout << "Q: " << question << std::endl;
        std::cout << "A: " << agent.ask(question) << std::endl;
    }
    return 0;
}

/// 拿不到 key 时的提示。必须说清楚三件事：文件叫什么、放哪个字段、
/// 以及为什么仓库里没有现成的（因为不能提交）。
std::string missingKeyMessage(const mcp::LlmConfig& llm) {
    std::string message = "没有找到 DeepSeek API key。\n";
    if (!llm.error.empty()) {
        message += llm.error + "\n";
    }
    message += "请把仓库根目录下的 deepseek.example.json 复制成 " +
               std::string(mcp::configFileName()) + "，并填入自己的 key：\n"
               "  { \"apiKey\": \"sk-...\", \"model\": \"deepseek-chat\" }\n"
               "该文件已在 .gitignore 中，不会被提交。也可以设环境变量 DEEPSEEK_API_KEY，"
               "或用 --key 临时传入。";
    return message;
}

void printUsage(const std::string& defaultServerPath) {
    std::cout << "用法: mcp-client-qt.exe [选项]\n"
              << "  --exe <路径>      server 可执行文件（默认 " << defaultServerPath << "）\n"
              << "  --config <路径>   大模型配置文件（默认依次找 exe 同目录、当前目录的 "
              << mcp::configFileName() << "）\n"
              << "  --key <key>       临时指定 DeepSeek API key，覆盖配置文件\n"
              << "  --cli [问题...]   不开窗口，在终端跑完问答后退出（用于自测，可多个问句）\n"
              << "  -h, --help        打印本帮助\n";
}

}  // namespace

class MainWindow : public QWidget {
    Q_OBJECT

public:
    MainWindow(std::string serverPath, mcp::LlmConfig llm, QWidget* parent = nullptr)
        : QWidget(parent), serverPath_(std::move(serverPath)), llm_(std::move(llm)) {
        buildUi();
        connectAgent();
    }

private:
    void buildUi() {
        setWindowTitle(QString::fromUtf8("天气助手 · DeepSeek + MCP"));
        resize(780, 600);

        // 中文默认字体在 Windows 上容易落到点阵宋体，显式指定雅黑。
        QFont uiFont(QString::fromUtf8("Microsoft YaHei UI"), 10);
        setFont(uiFont);

        output_ = new QTextEdit(this);
        output_->setReadOnly(true);
        output_->setAcceptRichText(true);
        output_->setFont(uiFont);

        input_ = new QTextEdit(this);
        input_->setFixedHeight(72);
        input_->setFont(uiFont);
        input_->setPlaceholderText(
            QString::fromUtf8("用自然语言提问，例如：武汉明天天气怎么样（Ctrl+Enter 发送）"));

        sendButton_ = new QPushButton(QString::fromUtf8("发送"), this);
        sendButton_->setDefault(true);
        QPushButton* clearButton = new QPushButton(QString::fromUtf8("清空对话"), this);
        status_ = new QLabel(QString::fromUtf8("就绪"), this);

        QHBoxLayout* buttons = new QHBoxLayout();
        buttons->addWidget(sendButton_);
        buttons->addWidget(clearButton);
        buttons->addStretch(1);
        buttons->addWidget(status_);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(QString::fromUtf8("对话记录"), this));
        layout->addWidget(output_, 1);
        layout->addWidget(new QLabel(QString::fromUtf8("你的问题"), this));
        layout->addWidget(input_);
        layout->addLayout(buttons);

        connect(sendButton_, &QPushButton::clicked, this, &MainWindow::onSend);
        connect(clearButton, &QPushButton::clicked, this, &MainWindow::onClear);
        connect(watcher_, &QFutureWatcher<QString>::finished, this, &MainWindow::onFinished);

        QShortcut* shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Return), this);
        connect(shortcut, &QShortcut::activated, this, &MainWindow::onSend);

        input_->setFocus();
    }

    void connectAgent() {
        if (!llm_.hasKey()) {
            appendLine(QString::fromUtf8("系统"), QString::fromStdString(missingKeyMessage(llm_)));
            sendButton_->setEnabled(false);
            status_->setText(QString::fromUtf8("缺少 API key"));
            return;
        }
        try {
            agent_ = std::make_unique<mcp::WeatherAgent>(serverPath_, llm_);
            agent_->connect();
            const mcp::McpClient::ServerInfo info = agent_->serverInfo();
            appendLine(QString::fromUtf8("系统"),
                       QString::fromUtf8("已连接 %1 v%2（协议 %3）；key 来自 %4。直接提问吧。")
                           .arg(QString::fromStdString(info.serverName),
                                QString::fromStdString(info.serverVersion),
                                QString::fromStdString(info.protocolVersion),
                                QString::fromStdString(llm_.source)));
        } catch (const std::exception& e) {
            appendLine(QString::fromUtf8("系统"),
                       QString::fromUtf8("连接 MCP server 失败：%1").arg(e.what()));
            sendButton_->setEnabled(false);
        }
    }

    void appendLine(const QString& who, const QString& text) {
        // 走富文本：多行回答里的 \n 在 HTML 里不换行，得换成 <br>。
        const QString body = text.toHtmlEscaped().replace("\n", "<br>");
        output_->append(QString("<b>%1：</b>%2").arg(who, body));
        output_->ensureCursorVisible();
    }

    void onSend() {
        const QString question = input_->toPlainText().trimmed();
        if (question.isEmpty() || !agent_) {
            return;
        }
        input_->clear();
        appendLine(QString::fromUtf8("你"), question);

        sendButton_->setEnabled(false);
        status_->setText(QString::fromUtf8("正在查询…"));

        mcp::WeatherAgent* agent = agent_.get();
        const std::string prompt = question.toStdString();
        // 捕获裸指针：agent_ 的生命周期由本窗口持有，且期间按钮已禁用、不会重入。
        QFuture<QString> future = QtConcurrent::run([agent, prompt]() -> QString {
            try {
                return QString::fromStdString(agent->ask(prompt));
            } catch (const std::exception& e) {
                return QString::fromUtf8("出错了：%1").arg(e.what());
            }
        });
        watcher_->setFuture(future);
    }

    void onFinished() {
        appendLine(QString::fromUtf8("助手"), watcher_->result());
        status_->setText(QString::fromUtf8("就绪"));
        sendButton_->setEnabled(true);
        input_->setFocus();
    }

    void onClear() {
        output_->clear();
        if (agent_) {
            agent_->reset();
        }
    }

    std::string serverPath_;
    mcp::LlmConfig llm_;
    std::unique_ptr<mcp::WeatherAgent> agent_;
    QTextEdit* output_ = nullptr;
    QTextEdit* input_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QLabel* status_ = nullptr;
    QFutureWatcher<QString>* watcher_ = new QFutureWatcher<QString>(this);
};

int main(int argc, char** argv) {
    mcp::useUtf8Console();

    std::string serverPath = mcp::executableDir() + "\\weather-mcp.exe";
    std::string configPath;
    std::string keyOverride;
    std::vector<std::string> cliQuestions;
    bool cliMode = false;
    bool wantHelp = false;

    // argv 在中文 Windows 上是 GBK，中文问句得先按 UTF-16 取回再转 UTF-8，
    // 否则塞进 JSON 请求体时会抛 invalid UTF-8。
    const std::vector<std::string> args = mcp::decodeCommandLineArgs(argc, argv);

    try {
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string arg = args[i];
            auto value = [&](const std::string& name) -> std::string {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error("选项 " + name + " 缺少取值");
                }
                return args[++i];
            };

            if (arg == "--cli") {
                cliMode = true;
                // 后面连续的非选项参数都当作问句，可以一次跑多轮验证上下文连贯。
                while (i + 1 < argc && argv[i + 1][0] != '-') {
                    cliQuestions.push_back(value(arg));
                }
            } else if (arg == "--exe") {
                serverPath = value(arg);
            } else if (arg == "--key") {
                keyOverride = value(arg);
            } else if (arg == "--config") {
                configPath = value(arg);
            } else if (arg == "-h" || arg == "--help") {
                wantHelp = true;
            } else {
                throw std::runtime_error("未知参数：" + arg + "（用 --help 看用法）");
            }
        }

        if (wantHelp) {
            printUsage(serverPath);
            return 0;
        }

        // key 只从外部进来：配置文件 / 环境变量 / --key。源码里没有任何默认值。
        mcp::LlmConfig llm = mcp::loadLlmConfig(configPath);
        if (!keyOverride.empty()) {
            llm.apiKey = keyOverride;
            llm.source = "命令行 --key";
        }

        if (cliMode) {
            if (!llm.hasKey()) {
                std::cerr << missingKeyMessage(llm) << std::endl;
                return 1;
            }
            std::cerr << "[qt-client] 使用配置：" << llm.source << std::endl;
            return runCli(serverPath, llm,
                          cliQuestions.empty() ? kDemoQuestions : cliQuestions);
        }

        QApplication app(argc, argv);
        MainWindow window(serverPath, llm);
        window.show();
        return app.exec();
    } catch (const std::exception& e) {
        std::cerr << "[qt-client] 失败：" << e.what() << std::endl;
        return 1;
    }
}

#include "qt_main.moc"
