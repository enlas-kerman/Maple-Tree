#include "mapleseed.h"
#include "ui_mainwindow.h"
#include "versioninfo.h"

MapleSeed* MapleSeed::self;

MapleSeed::MapleSeed(QWidget* parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(self = this);
    setWindowTitle("MapleSeed++ " + QString(GEN_VERSION_STRING));
    initialize();
}

MapleSeed::~MapleSeed()
{
    Gamepad::terminate();
    if (downloadManager)
    {
        delete downloadManager;
    }
    if (gameLibrary)
    {
        delete gameLibrary;
    }
    if (config)
    {
        delete config;
    }
    delete process;
    delete ui;
}

//https://stackoverflow.com/questions/34318934/qt-installer-framework-auto-update
void MapleSeed::checkUpdate()
{
    qInfo() << "Checking for update";

    QProcess process;
    process.start("maintenancetool --checkupdates");
    process.waitForFinished();

    if(process.error() != QProcess::UnknownError)
    {
        qDebug() << "Error checking for updates";
        return;
    }

    QByteArray data = process.readAllStandardOutput();

    if(data.isEmpty())
    {
        qDebug() << "No updates available";
        return;
    }

    qDebug() << data.constData();

    if (QMessageBox::information(this, "Update Available!!", "Would you like to update?", QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
    {
        QtConcurrent::run([=] { QProcess::startDetached("maintenancetool", QStringList() << "--updater"); });
        qApp->closeAllWindows();
    }
}

void MapleSeed::initialize()
{
    qInfo() << "Setting up enviornment variables";
    QtCompressor::self = new QtCompressor;

    defineActions();
    if (!config->load()) {
      config->save();
    }
    defaultConfiguration();

    gameLibrary->init(config->getBaseDirectory());
    on_actionGamepad_triggered(config->getKeyBool("Gamepad"));

    qInfo() << "Environment setup complete";
    checkUpdate();
}

void MapleSeed::defineActions()
{
    connect(QtCompressor::self, &QtCompressor::updateProgress, this, &MapleSeed::updateBaiscProgress);

    connect(config->decrypt, &Decrypt::decryptStarted, this, &MapleSeed::disableMenubar);
    connect(config->decrypt, &Decrypt::decryptFinished, this, &MapleSeed::enableMenubar);
    connect(config->decrypt, &Decrypt::progressReport, this, &MapleSeed::updateBaiscProgress);
    connect(config->decrypt, &Decrypt::progressReport2, this, &MapleSeed::updateProgress);

    connect(gameLibrary, &GameLibrary::progress, this, &MapleSeed::updateBaiscProgress);
    connect(gameLibrary, &GameLibrary::changed, this, &MapleSeed::updateListview);
    connect(gameLibrary, &GameLibrary::addTitle, this, &MapleSeed::updateTitleList);
    connect(gameLibrary, &GameLibrary::loadComplete, this, &MapleSeed::gameLibraryLoadComplete);

    connect(downloadQueue, &DownloadQueue::ObjectAdded, this, &MapleSeed::DownloadQueueAdd);
    //connect(downloadQueue, &DownloadQueue::ObjectFinished, this, &MapleSeed::DownloadQueueRemove);
    connect(downloadQueue, &DownloadQueue::QueueFinished, this, &MapleSeed::DownloadQueueFinished);
    connect(downloadQueue, &DownloadQueue::QueueProgress, this, &MapleSeed::updateDownloadProgress);
}

void MapleSeed::defaultConfiguration()
{
    ui->actionFullscreen->setChecked(config->getKeyBool("Fullscreen"));
    ui->actionIntegrateCemu->setChecked(config->getKeyBool("IntegrateCemu"));
    ui->checkBoxEShopTitles->setChecked(config->getKeyBool("eShopTitles"));
    ui->actionGamepad->setChecked(Gamepad::isEnabled = config->getKeyBool("Gamepad"));
    ui->actionDebug->setChecked(Debug::isEnabled = config->getKeyBool("DebugLogging"));
}

QDir* MapleSeed::selectDirectory()
{
  QFileDialog dialog;
  dialog.setFileMode(QFileDialog::DirectoryOnly);
  dialog.setOption(QFileDialog::ShowDirsOnly);
  if (dialog.exec())
  {
    QStringList entries(dialog.selectedFiles());
    return new QDir(entries.first());
  }
  return nullptr;
}

QFileInfo MapleSeed::selectFile(QString defaultDir)
{
    QFileDialog dialog;
    if (QDir(defaultDir).exists())
    {
        dialog.setDirectory(defaultDir);
    }
    dialog.setNameFilter("*.qta");
    if (dialog.exec())
    {
        QStringList entries(dialog.selectedFiles());
        return entries.first();
    }
    return QFileInfo();
}

void MapleSeed::CopyToClipboard(QString text)
{
    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(text);
    qInfo() << text << "copied to clipboard";
}

void MapleSeed::executeCemu(QString rpxPath)
{
    QFileInfo rpx(rpxPath);
    if (rpx.exists())
    {
        QString args("-g \"" + rpx.filePath() + "\"");
        if (config->getKeyBool("Fullscreen"))
        {
            args.append(" -f");
        }
        QString file(config->getKeyString("cemupath"));
        process->setWorkingDirectory(QFileInfo(file).dir().path());
        process->setNativeArguments(args);
        process->setProgram(file);
        process->start();
    }
}

bool MapleSeed::processActive()
{
    if (process->state() == process->NotRunning)
    {
        return false;
    }
    return true;
}

void MapleSeed::DownloadQueueAdd(QueueInfo *info)
{
    int row = ui->downloadQueue_tableWidget->rowCount();
    ui->downloadQueue_tableWidget->insertRow(row);
    ui->downloadQueue_tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->downloadQueue_tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->downloadQueue_tableWidget->setItem(row, 0, new QTableWidgetItem(info->name));
    ui->downloadQueue_tableWidget->setItem(row, 1, new QTableWidgetItem(config->size_human(info->totalSize)));
    ui->downloadQueue_tableWidget->setCellWidget(row, 2, info->pgbar);
}

void MapleSeed::DownloadQueueRemove(QueueInfo *info)
{
    auto item = ui->downloadQueue_tableWidget->findItems(info->name, Qt::MatchExactly);
    if (item.isEmpty()) {
        return;
    }

    auto watcher = new QFutureWatcher<void>;
    connect(watcher, &QFutureWatcher<void>::finished, this, [=]
    {
        int row = ui->downloadQueue_tableWidget->row(item.first());
        ui->downloadQueue_tableWidget->removeRow(row);
        delete watcher;
    });

    QFuture<void> future = QtConcurrent::run(&Decrypt::run, info->directory);
    watcher->setFuture(future);
    ui->downloadQueue_tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->downloadQueue_tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

void MapleSeed::DownloadQueueFinished(QList<QueueInfo*> history)
{
    for(auto info : history)
    {
        DownloadQueueRemove(info);
    }
}

void MapleSeed::gameUp(bool pressed)
{
    if (!pressed || processActive()) return;
    qDebug() << "row up";

    QListWidget *listWidget;
    if (ui->tabWidget->currentIndex() == 0) {
        listWidget = ui->listWidget;
    }
    else if (ui->tabWidget->currentIndex() == 1) {
        listWidget = ui->titlelistWidget;
    }
    else {
        return;
    }

    auto row = listWidget->currentRow();
    if (listWidget->currentRow() == 0)
    {
        row = listWidget->count()-1;
    }
    else {
        row -= 1;
        auto item(listWidget->item(row));
        while (item && item->isHidden()){
            item = listWidget->item(row -= 1);
        }
    }
    listWidget->setCurrentRow(row);
}

void MapleSeed::gameDown(bool pressed)
{
    if (!pressed || processActive()) return;
    qDebug() << "row down";

    QListWidget *listWidget;
    if (ui->tabWidget->currentIndex() == 0) {
        listWidget = ui->listWidget;
    }
    else if (ui->tabWidget->currentIndex() == 1) {
        listWidget = ui->titlelistWidget;
    }
    else {
        return;
    }

    auto row = listWidget->currentRow();
    if (listWidget->currentRow() == listWidget->count()-1)
    {
        row = 0;
    }
    else {
        row += 1;
        auto item(listWidget->item(row));
        while (item && item->isHidden()){
            item = listWidget->item(row += 1);
        }
    }
    listWidget->setCurrentRow(row);
}

void MapleSeed::gameStart(bool pressed)
{
    if (!pressed || processActive()) return;
    qDebug() << "game start";

    auto item = ui->listWidget->selectedItems().first();
    auto titleInfoItem = reinterpret_cast<TitleInfoItem*>(item);
    if (titleInfoItem->getItem())
    {
        executeCemu(titleInfoItem->getItem()->rpx);
    }
}

void MapleSeed::gameClose(bool pressed)
{
    if (!pressed || !processActive()) return;

    process->terminate();
}

void MapleSeed::prevTab(bool pressed)
{
    if (!pressed || processActive()) return;
    qDebug() << "prev tab";

    int index = ui->tabWidget->currentIndex();
    ui->tabWidget->setCurrentIndex(index-1);
}

void MapleSeed::nextTab(bool pressed)
{
    if (!pressed || processActive()) return;
    qDebug() << "next tab";

    int index = ui->tabWidget->currentIndex();
    ui->tabWidget->setCurrentIndex(index+1);
}

void MapleSeed::messageLog(QString msg)
{
    if (mutex.tryLock(100))
    {
        if (ui && ui->statusbar)
        {
            ui->statusbar->showMessage(msg);
        }
        mutex.unlock();
    }

    if (Debug::isEnabled)
    {
        QFile file(QCoreApplication::applicationName() + ".log");
        if (!file.open(QIODevice::Append))
        {
          qWarning("Couldn't open file.");
          return;
        }
        QString log(QDateTime::currentDateTime().toString("[MMM dd, yyyy HH:mm:ss ap] ") + msg + "\n");
        file.write(log.toLatin1());
        file.close();
    }
}

void MapleSeed::gameLibraryLoadComplete()
{
    on_checkBoxEShopTitles_stateChanged(config->getKeyBool("eShopTitles"));
}

void MapleSeed::SelectionChanged(QListWidget* listWidget)
{
    auto items = listWidget->selectedItems();
    if (items.isEmpty())
        return;

    TitleInfoItem* tii = reinterpret_cast<TitleInfoItem*>(items.first());
    ui->label->setPixmap(QPixmap(tii->getItem()->titleInfo->getCoverArtPath()));
}

void MapleSeed::showContextMenu(QListWidget* list, const QPoint& pos)
{
  QPoint globalPos = list->mapToGlobal(pos);
  if (list->selectedItems().isEmpty()) {
    return;
  }
  auto itm = list->selectedItems().first();
  auto tii = reinterpret_cast<TitleInfoItem*>(itm);
  auto entry = tii->getItem();
  TitleInfo* titleInfo = entry->titleInfo;
  if (!tii->getItem()) {
      return;
  }
  QString name(QFileInfo(tii->getItem()->directory).baseName());
  if (name.isEmpty()) {
      name = titleInfo->getName();
  }

  QMenu menu;

  if (QFileInfo(entry->rpx).exists())
  {
      menu.addAction("[Play] " + name, this, [=]
      { executeCemu(entry->rpx); })->setEnabled(true);
  }
  else
  {
      menu.addAction(name, this, [=]{})->setEnabled(false);
  }

  if (QFileInfo(entry->rpx).exists() && config->getIntegrateCemu())
  {
      menu.addSeparator();
      menu.addAction("Export Save Data", this, [&]
      {
          QDir dir = config->getBaseDirectory();
          if (dir.exists())
          {
              entry->backupSave(dir.filePath("Backup"));
          }
          else
          {
              qWarning() << "Save data export failed, base directory not valid" << config->getBaseDirectory();
          }
      })->setEnabled(true);
      menu.addAction("Import Save Data", this, [&]
      {
          QString dir = QDir(config->getBaseDirectory()+"/Backup/"+titleInfo->getFormatName()).absolutePath();
          if (QDir().mkpath(dir))
          {
              QFileInfo fileInfo = selectFile(dir);
              if (fileInfo.exists())
              {
                  entry->ImportSave(fileInfo.absoluteFilePath());
              }
          }
      })->setEnabled(true);
      menu.addAction("Purge Save Data", this, [&]
      {
          QMessageBox::StandardButton reply;
          reply = QMessageBox::question(this, titleInfo->getFormatName(), "Purge Save Data?", QMessageBox::Yes|QMessageBox::No);
          if (reply == QMessageBox::Yes)
          {
              QString saveDir = LibraryEntry::initSave(titleInfo->getID());
              QDir meta = QDir(saveDir).filePath("meta");
              QDir user = QDir(saveDir).filePath("user");
              if (!meta.removeRecursively() || !user.removeRecursively())
              {
                  qWarning() << "Purge Save Data: failed";
              }
          }
      })->setEnabled(true);
  }

  if (!QFileInfo(entry->metaxml).exists())
  {
      menu.addSeparator();
      TitleItem* ti_ui = new TitleItem(this);
      menu.addAction("Add Entry", this, [&]
      {
          if (ti_ui->add(titleInfo->getID()) == QDialog::Accepted){
              auto le = new LibraryEntry(std::move(ti_ui->getInfo()));
              this->updateTitleList(std::move(le));
          }
          delete ti_ui;
      });
      menu.addAction("Delete Entry", this, [=]
      {
          QMessageBox::StandardButton reply;
          reply = QMessageBox::question(this, titleInfo->getFormatName(), "Delete Entry?", QMessageBox::Yes|QMessageBox::No);
          if (reply == QMessageBox::Yes)
          {
              gameLibrary->database.remove(titleInfo->getID());
              if (gameLibrary->saveDatabase())
              {
                  delete ui->titlelistWidget->takeItem(ui->titlelistWidget->row(itm));
              }
          }
      });
      menu.addAction("Modify Entry", this, [&]
      {
          if (ti_ui->modify(tii->getItem()->titleInfo->getID()) == QDialog::Accepted){
              tii->setText(ti_ui->getInfo()->getFormatName());
              tii->getItem()->titleInfo->info = ti_ui->getInfo()->info;
              gameLibrary->database[ti_ui->getInfo()->getID()] = std::move(ti_ui->getInfo());
              gameLibrary->saveDatabase();
              list->editItem(tii);
          }
          delete ti_ui;
      });
  }

  menu.addSeparator();
  if (TitleInfo::ValidId(titleInfo->getID().replace(7, 1, '0'))) {
      menu.addAction("Download Game", this, [=] { titleInfo->download(); });
  }
  if (TitleInfo::ValidId(titleInfo->getID().replace(7, 1, 'c'))) {
      menu.addAction("Download DLC", this, [=] { titleInfo->downloadDlc(); });
  }
  if (TitleInfo::ValidId(titleInfo->getID().replace(7, 1, 'e'))) {
      menu.addAction("Download Patch", this, [=] { titleInfo->downloadPatch(); });
  }

  menu.addSeparator();
  if (QFile(QDir(titleInfo->getDirectory()).filePath("tmd")).exists() && QFile(QDir(titleInfo->getDirectory()).filePath("cetk")).exists())
      menu.addAction("Decrypt Content", this, [=] { QtConcurrent::run([=] {titleInfo->decryptContent(); }); });
  menu.addAction("Copy ID to Clipboard", this, [=] { CopyToClipboard(titleInfo->getID()); });

  menu.setEnabled(ui->menubar->isEnabled());
  menu.exec(globalPos);
}

void MapleSeed::disableMenubar()
{
    ui->menubar->setEnabled(false);
}

void MapleSeed::enableMenubar()
{
    ui->menubar->setEnabled(true);
}

void MapleSeed::updateListview(LibraryEntry* entry)
{
    TitleInfoItem* tii = new TitleInfoItem(entry);
    QString name(tii->getItem()->titleInfo->getFormatName());
    if (ui->listWidget->findItems(name, Qt::MatchExactly).isEmpty()){
        ui->listWidget->addItem(tii);
    }
}

void MapleSeed::updateTitleList(LibraryEntry* entry)
{
    TitleInfoItem* tii = new TitleInfoItem(entry);
    tii->setText(tii->getItem()->titleInfo->getFormatName());
    this->ui->titlelistWidget->addItem(tii);
}

void MapleSeed::updateDownloadProgress(qint64 bytesReceived, qint64 bytesTotal, QTime qtime)
{
    float percent = (static_cast<float>(bytesReceived) / static_cast<float>(bytesTotal)) * 100;

    this->ui->progressBar->setRange(0, 100);
    this->ui->progressBar->setValue(static_cast<int>(percent));

    double speed = bytesReceived * 1000.0 / qtime.elapsed();
    QString unit;
    if (speed < 1024) {
      unit = "bytes/sec";
    } else if (speed < 1024 * 1024) {
      speed /= 1024;
      unit = "kB/s";
    } else {
      speed /= 1024 * 1024;
      unit = "MB/s";
    }

    this->ui->progressBar->setFormat("%p% " +
        config->size_human(bytesReceived) + " / " + config->size_human(bytesTotal) + " | " +
        QString::fromLatin1("%1 %2").arg(speed, 3, 'f', 1).arg(unit));
}

void MapleSeed::updateProgress(qint64 min, qint64 max, int curfile, int maxfiles)
{
    float per = (static_cast<float>(min) / static_cast<float>(max)) * 100;
    this->ui->progressBar->setRange(0, static_cast<int>(100));
    this->ui->progressBar->setValue(static_cast<int>(per));
    this->ui->progressBar->setFormat(QString::number(per, 'G', 3) + "% " +
        config->size_human(min) + " / " + config->size_human(max) + " | " +
        QString::number(curfile) + " / " + QString::number(maxfiles) + " files");
}

void MapleSeed::updateBaiscProgress(qint64 min, qint64 max)
{
  this->ui->progressBar->setRange(0, static_cast<int>(max));
  this->ui->progressBar->setValue(static_cast<int>(min));
  this->ui->progressBar->setFormat("%p% / %v of %m");
}

void MapleSeed::filter(QString region, QString filter_string)
{
    if (!filter_string.isEmpty())
        qInfo() << "filter:" << filter_string;

    for (int row(0); row < ui->titlelistWidget->count(); row++)
        ui->titlelistWidget->item(row)->setHidden(true);

    QString searchString;
    auto matches = QList<QListWidgetItem*>();

    if (filter_string.isEmpty()) {
        searchString.append("*" + region + "* *");
        matches.append(ui->titlelistWidget->findItems(searchString, Qt::MatchFlag::MatchWildcard | Qt::MatchFlag::MatchCaseSensitive));
    }
    else {
        searchString.append("*" + region + "*" + filter_string + "*");
        matches.append(ui->titlelistWidget->findItems(searchString, Qt::MatchFlag::MatchWildcard));
    }

    QtConcurrent::blockingMapped(matches, &MapleSeed::processItemFilter);
}

QListWidgetItem* MapleSeed::processItemFilter(QListWidgetItem *item)
{
    if (self->mutex.tryLock(100))
    {
        if (Configuration::self->getKeyBool("eShopTitles")){
            item->setHidden(false);
        }else{
            auto tii = reinterpret_cast<TitleInfoItem*>(item);
            if (tii->getItem()->titleInfo->coverExists()){
                item->setHidden(false);
            }
        }
        self->mutex.unlock();
    }
    return item;
}

void MapleSeed::on_actionQuit_triggered()
{
    if (QMessageBox::question(this, "Exit", "Exit Program?", QMessageBox::Yes|QMessageBox::No) == QMessageBox::Yes)
    {
      qApp->closeAllWindows();
    }
}

void MapleSeed::on_actionChangeLibrary_triggered()
{
    QDir* dir = selectDirectory();
    if (dir == nullptr)
      return;
    QString directory(dir->path());
    delete dir;

    ui->listWidget->clear();
    config->setBaseDirectory(directory);
    QtConcurrent::run([=] { gameLibrary->setupLibrary(directory, true); });
}

void MapleSeed::on_actionDecryptContent_triggered()
{
    QDir* dir = this->selectDirectory();
    if (dir == nullptr)
      return;

    if (!QFileInfo(dir->filePath("tmd")).exists()) {
      QMessageBox::critical(this, "Missing file", +"Missing: " + dir->filePath("/tmd"));
      return;
    }
    if (!QFileInfo(dir->filePath("cetk")).exists()) {
      QMessageBox::critical(this, "Missing file", +"Missing: " + dir->filePath("/cetk"));
      return;
    }

    QString path = dir->path();
    delete dir;
    qInfo() << "Decrypting" << path;
    QtConcurrent::run([ = ] { config->decrypt->start(path); });
}

void MapleSeed::on_actionIntegrateCemu_triggered(bool checked)
{
    config->setKeyBool("IntegrateCemu", checked);
    QString cemulocation(config->getCemuPath());
    if (checked && !QFile(cemulocation).exists())
    {
      QFileDialog dialog;
      dialog.setNameFilter("cemu.exe");
      dialog.setFileMode(QFileDialog::ExistingFile);
      if (dialog.exec())
      {
        QStringList files(dialog.selectedFiles());
        config->setKey("CemuPath", QFileInfo(files.first()).absoluteFilePath());
      }
      else
      {
          ui->actionIntegrateCemu->setChecked(false);
          return;
      }
    }
    if (checked)
    {
        auto str("Save Data exports WILL NOT work with any other save data tool/program. DO NOT change the default export file name.");
        QMessageBox::information(this, "Warning!!!!!!",  str);
    }
}

void MapleSeed::on_actionRefreshLibrary_triggered()
{
    QFile(Configuration::self->getLibPath()).remove();
    ui->listWidget->clear();
    QtConcurrent::run([=] { gameLibrary->setupLibrary(true); });
}

void MapleSeed::on_actionClearSettings_triggered()
{
    auto reply = QMessageBox::information(this, "Warning!!!", "Do you want to delete all settings and temporary files?\nThis application will close.", QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes)
    {
      QDir dir(config->getPersistentDirectory());
      delete gameLibrary;
      delete config;
      QFile(dir.filePath("settings.json")).remove();
      QFile(dir.filePath("library.json")).remove();
      QApplication::quit();
    }
}

void MapleSeed::on_actionCovertArt_triggered()
{
    QDir directory("covers");
    QString fileName("covers.qta");

    if (!QFile(fileName).exists()) {
        downloadManager->downloadSingle(QUrl("http://pixxy.in/mapleseed/covers.qta"), fileName);
    }

    if (!directory.exists()) {
        QtConcurrent::run([=] { QtCompressor::decompress(fileName, directory.absolutePath()); });
    }
}

void MapleSeed::on_actionCompress_triggered()
{
    QDir* directory(this->selectDirectory());
    if (directory) {
        QString path(directory->absolutePath());
        QtConcurrent::run([=] { QtCompressor::compress(path, path + ".qta"); });
        delete directory;
    }
}

void MapleSeed::on_actionDecompress_triggered()
{
    QFileInfo info(this->selectFile());
    if (info.exists()) {
        QString filename = info.absoluteFilePath();
        QString dir = info.absoluteDir().filePath(info.baseName());
        QtConcurrent::run([=] { QtCompressor::decompress(filename, dir); });
    }
}

void MapleSeed::on_actionDownload_triggered()
{
    bool ok;
    QString value = QInputDialog::getText(this, "Download Content", "Insert title id of desired content below.", QLineEdit::Normal, nullptr, &ok);
    if (!ok){
        return;
    }
    if (value.contains('-')){
        value.remove('-');
    }
    if (value.isEmpty() || value.length() != 16) {
        QMessageBox::information(this, "Download Title Error", "Invalid title id");
        return;
    }
    TitleInfo* titleinfo = TitleInfo::Create(value, gameLibrary->baseDirectory);
    titleinfo->download();
}

void MapleSeed::on_listWidget_itemDoubleClicked(QListWidgetItem *item)
{
    if (item == nullptr || !ui->actionIntegrateCemu->isChecked())
      return;
    auto titleInfoItem = reinterpret_cast<TitleInfoItem*>(item);
    executeCemu(titleInfoItem->getItem()->rpx);
}

void MapleSeed::on_listWidget_itemSelectionChanged()
{
    return SelectionChanged(ui->listWidget);
}

void MapleSeed::on_listWidget_customContextMenuRequested(const QPoint &pos)
{
    return showContextMenu(ui->listWidget, pos);
}

void MapleSeed::on_titlelistWidget_itemSelectionChanged()
{
    return SelectionChanged(ui->titlelistWidget);
}

void MapleSeed::on_titlelistWidget_customContextMenuRequested(const QPoint &pos)
{
    return showContextMenu(ui->titlelistWidget, pos);
}

void MapleSeed::on_searchInput_textEdited(const QString &arg1)
{
    return filter(ui->regionBox->currentText(), arg1);
}

void MapleSeed::on_regionBox_currentTextChanged(const QString &arg1)
{
    return filter(arg1, ui->searchInput->text());
}

void MapleSeed::on_checkBoxEShopTitles_stateChanged(int arg1)
{
    config->setKeyBool("eShopTitles", arg1);
    filter(ui->regionBox->currentText(), ui->searchInput->text());
}

void MapleSeed::on_actionGamepad_triggered(bool checked)
{
    config->setKeyBool("Gamepad", checked);

    if (Gamepad::instance == nullptr)
    {
        Gamepad::instance = new Gamepad;
        QtConcurrent::run([=] { Gamepad::instance->init(); });
        connect(Gamepad::instance, &Gamepad::gameUp, this, &MapleSeed::gameUp);
        connect(Gamepad::instance, &Gamepad::gameDown, this, &MapleSeed::gameDown);
        connect(Gamepad::instance, &Gamepad::gameStart, this, &MapleSeed::gameStart);
        connect(Gamepad::instance, &Gamepad::gameClose, this, &MapleSeed::gameClose);
        connect(Gamepad::instance, &Gamepad::prevTab, this, &MapleSeed::prevTab);
        connect(Gamepad::instance, &Gamepad::nextTab, this, &MapleSeed::nextTab);
    }

    if (checked){
        Gamepad::enable();
    }else {
        Gamepad::disable();
    }
}

void MapleSeed::on_actionDebug_triggered(bool checked)
{
    config->setKeyBool("DebugLogging", Debug::isEnabled = checked);
}

void MapleSeed::on_actionOpen_Log_triggered()
{
    QString logFile(QDir(QCoreApplication::applicationName() + ".log").absolutePath());
    QDesktopServices::openUrl(QUrl(QCoreApplication::applicationName() + ".log"));
}

void MapleSeed::on_actionFullscreen_triggered(bool checked)
{
    config->setKeyBool("Fullscreen", checked);
}
