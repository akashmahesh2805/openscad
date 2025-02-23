#include "gui/TabManager.h"

#include <QApplication>
#include <QPoint>
#include <QTabBar>
#include <QWidget>
#include <cassert>
#include <functional>
#include <exception>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QSaveFile>
#include <QShortcut>
#include <QTextStream>
#include <QMessageBox>
#include <QFileDialog>
#include <QClipboard>
#include <QDesktopServices>
#include <Qsci/qscicommand.h>
#include <Qsci/qscicommandset.h>

#include "gui/Editor.h"
#include "gui/TabWidget.h"
#include "gui/ScintillaEditor.h"
#include "gui/Preferences.h"
#include "gui/MainWindow.h"

#include <cstddef>

TabManager::TabManager(MainWindow *o, const QString& filename)
{
  par = o;

  tabWidget = new TabWidget();
  tabWidget->setAutoHide(true);
  tabWidget->setExpanding(false);
  tabWidget->setTabsClosable(true);
  tabWidget->setMovable(true);
  tabWidget->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(tabWidget, &TabWidget::currentTabChanged, this, &TabManager::tabSwitched);
  connect(tabWidget, &TabWidget::tabCloseRequested, this, &TabManager::closeTabRequested);
  connect(tabWidget, &TabWidget::tabCountChanged, this, &TabManager::tabCountChanged);
  connect(tabWidget, &TabWidget::middleMouseClicked, this, &TabManager::middleMouseClicked);
  connect(tabWidget, &TabWidget::customContextMenuRequested, this, &TabManager::showTabHeaderContextMenu);

  createTab(filename);

  connect(tabWidget, &TabWidget::currentTabChanged, this, &TabManager::stopAnimation);
  connect(tabWidget, &TabWidget::currentTabChanged, this, &TabManager::updateFindState);

  connect(par, &MainWindow::highlightError, this, &TabManager::highlightError);
  connect(par, &MainWindow::unhighlightLastError, this, &TabManager::unhighlightLastError);

  connect(par->editActionUndo, &QAction::triggered, this, &TabManager::undo);
  connect(par->editActionRedo, &QAction::triggered, this, &TabManager::redo);
  connect(par->editActionRedo_2, &QAction::triggered, this, &TabManager::redo);
  connect(par->editActionCut, &QAction::triggered, this, &TabManager::cut);
  connect(par->editActionPaste, &QAction::triggered, this, &TabManager::paste);

  connect(par->editActionIndent, &QAction::triggered, this, &TabManager::indentSelection);
  connect(par->editActionUnindent, &QAction::triggered, this, &TabManager::unindentSelection);
  connect(par->editActionComment, &QAction::triggered, this, &TabManager::commentSelection);
  connect(par->editActionUncomment, &QAction::triggered, this, &TabManager::uncommentSelection);

  connect(par->editActionToggleBookmark, &QAction::triggered, this, &TabManager::toggleBookmark);
  connect(par->editActionNextBookmark, &QAction::triggered, this, &TabManager::nextBookmark);
  connect(par->editActionPrevBookmark, &QAction::triggered, this, &TabManager::prevBookmark);
  connect(par->editActionJumpToNextError, &QAction::triggered, this, &TabManager::jumpToNextError);
}

QWidget *TabManager::getTabHeader()
{
  assert(tabWidget != nullptr);
  return tabWidget;
}

QWidget *TabManager::getTabContent()
{
  assert(tabWidget != nullptr);
  return tabWidget->getContentWidget();
}

void TabManager::tabSwitched(int x)
{
  assert(tabWidget != nullptr);
  editor = (EditorInterface *)tabWidget->widget(x);
  par->activeEditor = editor;
  par->parameterDock->setWidget(editor->parameterWidget);

  par->editActionUndo->setEnabled(editor->canUndo());
  par->changedTopLevelEditor(par->editorDock->isFloating());
  par->changedTopLevelConsole(par->consoleDock->isFloating());
  par->parameterTopLevelChanged(par->parameterDock->isFloating());
  par->setWindowTitle(tabWidget->tabText(x).replace("&&", "&"));

  for (int idx = 0; idx < tabWidget->count(); ++idx) {
    QWidget *button = tabWidget->tabButton(idx, QTabBar::RightSide);
    if (button) {
      button->setVisible(idx == x);
    }
  }
}

void TabManager::middleMouseClicked(int x)
{
  if (x < 0) {
    createTab("");
  } else {
    closeTabRequested(x);
  }
}

void TabManager::closeTabRequested(int x)
{
  assert(tabWidget != nullptr);
  if (!maybeSave(x)) return;

  auto *temp = (EditorInterface *)tabWidget->widget(x);
  editorList.remove(temp);
  tabWidget->removeTab(x);
  tabWidget->fireTabCountChanged();

  delete temp->parameterWidget;
  delete temp;
}

void TabManager::closeCurrentTab()
{
  assert(tabWidget != nullptr);

  /* Close tab or close the current window if only one tab is open. */
  if (tabWidget->count() > 1) this->closeTabRequested(tabWidget->currentIndex());
  else par->close();
}

void TabManager::nextTab()
{
  assert(tabWidget != nullptr);
  tabWidget->setCurrentIndex((tabWidget->currentIndex() + 1) % tabWidget->count());
}

void TabManager::prevTab()
{
  assert(tabWidget != nullptr);
  tabWidget->setCurrentIndex((tabWidget->currentIndex() + tabWidget->count() - 1) % tabWidget->count());
}

void TabManager::actionNew()
{
  if (par->windowActionHideEditor->isChecked()) par->windowActionHideEditor->trigger(); //if editor hidden, make it visible
  createTab("");
}

void TabManager::open(const QString& filename)
{
  assert(!filename.isEmpty());

  for (auto edt: editorList) {
    if (filename == edt->filepath) {
      tabWidget->setCurrentWidget(tabWidget->indexOf(edt));
      return;
    }
  }

  if (editor->filepath.isEmpty() && !editor->isContentModified() && !editor->parameterWidget->isModified()) {
    openTabFile(filename);
  } else {
    createTab(filename);
  }
}

void TabManager::createTab(const QString& filename)
{
  assert(par != nullptr);

  editor = new ScintillaEditor(tabWidget);
  Preferences::create(editor->colorSchemes()); // needs to be done only once, however handled
  par->activeEditor = editor;
  editor->parameterWidget = new ParameterWidget(par->parameterDock);
  connect(editor->parameterWidget, &ScintillaEditor::parametersChanged, par, &MainWindow::actionRenderPreview);
  par->parameterDock->setWidget(editor->parameterWidget);

  // clearing default mapping of keyboard shortcut for font size
  QsciCommandSet *qcmdset = ((ScintillaEditor *)editor)->qsci->standardCommands();
  QsciCommand *qcmd = qcmdset->boundTo(Qt::ControlModifier | Qt::Key_Plus);
  qcmd->setKey(0);
  qcmd = qcmdset->boundTo(Qt::ControlModifier | Qt::Key_Minus);
  qcmd->setKey(0);

  connect(editor, &ScintillaEditor::uriDropped, par, &MainWindow::handleFileDrop);
  connect(editor, &ScintillaEditor::previewRequest, par, &MainWindow::actionRenderPreview);
  connect(editor, &ScintillaEditor::showContextMenuEvent, this, &TabManager::showContextMenuEvent);
  connect(editor, &EditorInterface::focusIn, this, [=]() { par->setLastFocus(editor); });

  connect(Preferences::inst(), &Preferences::editorConfigChanged, editor, &ScintillaEditor::applySettings);
  connect(Preferences::inst(), &Preferences::autocompleteChanged, editor, &ScintillaEditor::onAutocompleteChanged);
  connect(Preferences::inst(), &Preferences::characterThresholdChanged, editor, &ScintillaEditor::onCharacterThresholdChanged);
  ((ScintillaEditor *)editor)->public_applySettings();
  editor->addTemplate();

  connect(par->editActionZoomTextIn, &QAction::triggered, editor, &ScintillaEditor::zoomIn);
  connect(par->editActionZoomTextOut, &QAction::triggered, editor, &ScintillaEditor::zoomOut);

  connect(editor, &ScintillaEditor::contentsChanged, this, &TabManager::updateActionUndoState);
  connect(editor, &ScintillaEditor::contentsChanged, par, &MainWindow::editorContentChanged);
  connect(editor, &ScintillaEditor::contentsChanged, this, &TabManager::setContentRenderState);
  connect(editor, &ScintillaEditor::modificationChanged, this, &TabManager::setTabModified);
  connect(editor->parameterWidget, &ParameterWidget::modificationChanged, [editor = this->editor, this] {
    setTabModified(editor);
  });

  connect(Preferences::inst(), &Preferences::fontChanged, editor, &ScintillaEditor::initFont);
  connect(Preferences::inst(), &Preferences::syntaxHighlightChanged, editor, &ScintillaEditor::setHighlightScheme);
  editor->initFont(Preferences::inst()->getValue("editor/fontfamily").toString(), Preferences::inst()->getValue("editor/fontsize").toUInt());
  editor->setHighlightScheme(Preferences::inst()->getValue("editor/syntaxhighlight").toString());

  connect(editor, &ScintillaEditor::hyperlinkIndicatorClicked, this, &TabManager::onHyperlinkIndicatorClicked);

  int idx = tabWidget->addTab(editor, _("Untitled.scad"));
  if (!editorList.isEmpty()) {
    tabWidget->setCurrentWidget(idx); // to prevent emitting of currentTabChanged signal twice for first tab
  }

  editorList.insert(editor);
  if (!filename.isEmpty()) {
    openTabFile(filename);
  } else {
    setTabName("");
  }
  par->updateRecentFileActions();
}

size_t TabManager::count()
{
  return tabWidget->count();
}

void TabManager::highlightError(int i)
{
  editor->highlightError(i);
}

void TabManager::unhighlightLastError()
{
  editor->unhighlightLastError();
}

void TabManager::undo()
{
  editor->undo();
}

void TabManager::redo()
{
  editor->redo();
}

void TabManager::cut()
{
  editor->cut();
}

void TabManager::copy()
{
  editor->copy();
}

void TabManager::paste()
{
  editor->paste();
}

void TabManager::indentSelection()
{
  editor->indentSelection();
}

void TabManager::unindentSelection()
{
  editor->unindentSelection();
}

void TabManager::commentSelection()
{
  editor->commentSelection();
}

void TabManager::uncommentSelection()
{
  editor->uncommentSelection();
}

void TabManager::toggleBookmark()
{
  editor->toggleBookmark();
}

void TabManager::nextBookmark()
{
  editor->nextBookmark();
}

void TabManager::prevBookmark()
{
  editor->prevBookmark();
}

void TabManager::jumpToNextError()
{
  editor->jumpToNextError();
}

void TabManager::setFocus()
{
  editor->setFocus();
}

void TabManager::updateActionUndoState()
{
  par->editActionUndo->setEnabled(editor->canUndo());
}

void TabManager::onHyperlinkIndicatorClicked(int val)
{
  const QString filename = QString::fromStdString(editor->indicatorData[val].path);
  this->open(filename);
}

void TabManager::applyAction(QObject *object, const std::function<void(int, EditorInterface *)>& func)
{
  auto *action = dynamic_cast<QAction *>(object);
  if (action == nullptr) {
    return;
  }
  bool ok;
  int idx = action->data().toInt(&ok);
  if (!ok) {
    return;
  }

  auto *edt = (EditorInterface *)tabWidget->widget(idx);
  if (edt == nullptr) {
    return;
  }

  func(idx, edt);
}

void TabManager::copyFileName()
{
  applyAction(QObject::sender(), [](int, EditorInterface *edt){
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(QFileInfo(edt->filepath).fileName());
  });
}

void TabManager::copyFilePath()
{
  applyAction(QObject::sender(), [](int, EditorInterface *edt){
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(edt->filepath);
  });
}

void TabManager::openFolder()
{
  applyAction(QObject::sender(), [](int, EditorInterface *edt){
    auto dir = QFileInfo(edt->filepath).dir();
    if (dir.exists()) {
      QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
    }
  });
}

void TabManager::closeTab()
{
  applyAction(QObject::sender(), [this](int idx, EditorInterface *){
    closeTabRequested(idx);
  });
}

void TabManager::showContextMenuEvent(const QPoint& pos)
{
  auto menu = editor->createStandardContextMenu();

  menu->addSeparator();
  menu->addAction(par->editActionFind);
  menu->addAction(par->editActionFindNext);
  menu->addAction(par->editActionFindPrevious);
  menu->addSeparator();
  menu->addAction(par->editActionInsertTemplate);
  menu->addAction(par->editActionFoldAll);
  menu->exec(editor->mapToGlobal(pos));

  delete menu;
}

void TabManager::showTabHeaderContextMenu(const QPoint& pos)
{
  int idx = tabWidget->tabAt(pos);
  if (idx < 0) {
    return;
  }

  auto *edt = (EditorInterface *)tabWidget->widget(idx);

  auto *copyFileNameAction = new QAction(tabWidget);
  copyFileNameAction->setData(idx);
  copyFileNameAction->setEnabled(!edt->filepath.isEmpty());
  copyFileNameAction->setText(_("Copy file name"));
  connect(copyFileNameAction, &QAction::triggered, this, &TabManager::copyFileName);

  auto *copyFilePathAction = new QAction(tabWidget);
  copyFilePathAction->setData(idx);
  copyFilePathAction->setEnabled(!edt->filepath.isEmpty());
  copyFilePathAction->setText(_("Copy full path"));
  connect(copyFilePathAction, &QAction::triggered, this, &TabManager::copyFilePath);

  auto *openFolderAction = new QAction(tabWidget);
  openFolderAction->setData(idx);
  openFolderAction->setEnabled(!edt->filepath.isEmpty());
  openFolderAction->setText(_("Open folder"));
  connect(openFolderAction, &QAction::triggered, this, &TabManager::openFolder);

  auto *closeAction = new QAction(tabWidget);
  closeAction->setData(idx);
  closeAction->setText(_("Close Tab"));
  connect(closeAction, &QAction::triggered, this, &TabManager::closeTab);

  QMenu menu;
  menu.addAction(copyFileNameAction);
  menu.addAction(copyFilePathAction);
  menu.addSeparator();
  menu.addAction(openFolderAction);
  menu.addSeparator();
  menu.addAction(closeAction);

  int x1, y1, x2, y2;
  tabWidget->tabRect(idx).getCoords(&x1, &y1, &x2, &y2);
  menu.exec(tabWidget->mapToGlobal(QPoint(x1, y2)));
}

void TabManager::setContentRenderState() //since last render
{
  editor->contentsRendered = false; //since last render
  editor->parameterWidget->setEnabled(false);
}

void TabManager::stopAnimation()
{
  par->animateWidget->pauseAnimation();
  par->animateWidget->e_tval->setText("");
}

void TabManager::updateFindState()
{
  if (editor->findState == TabManager::FIND_REPLACE_VISIBLE) par->showFindAndReplace();
  else if (editor->findState == TabManager::FIND_VISIBLE) par->showFind();
  else par->hideFind();
}

void TabManager::setTabModified(EditorInterface *edt)
{
  QString fname = _("Untitled.scad");
  QString fpath = fname;
  if (!edt->filepath.isEmpty()) {
    QFileInfo fileinfo(edt->filepath);
    fname = fileinfo.fileName();
    fpath = fileinfo.filePath();
  }

  if (edt->isContentModified() || edt->parameterWidget->isModified()) {
    fname += "*";
  }

  if (edt == editor) {
    par->setWindowTitle(fname);
  }
  tabWidget->setTabText(tabWidget->indexOf(edt), fname.replace("&", "&&"));
  tabWidget->setTabToolTip(tabWidget->indexOf(edt), fpath);
}

void TabManager::openTabFile(const QString& filename)
{
  par->setCurrentOutput();
  editor->setPlainText("");

  QFileInfo fileinfo(filename);
  const auto suffix = fileinfo.suffix().toLower();
  const auto knownFileType = par->knownFileExtensions.contains(suffix);
  const auto cmd = par->knownFileExtensions[suffix];
  if (knownFileType && cmd.isEmpty()) {
    setTabName(filename);
    editor->parameterWidget->readFile(fileinfo.absoluteFilePath());
    par->updateRecentFiles(filename);
  } else {
    setTabName(nullptr);
    editor->setPlainText(cmd.arg(filename));
  }
  par->fileChangedOnDisk(); // force cached autoReloadId to update
  bool opened = refreshDocument();

  if (opened) { // only try to parse if the file opened
    par->hideCurrentOutput(); // Initial parse for customizer, hide any errors to avoid duplication
    try {
      par->parseTopLevelDocument();
    } catch (const HardWarningException&) {
      par->exceptionCleanup();
    } catch (const std::exception& ex) {
      par->UnknownExceptionCleanup(ex.what());
    } catch (...) {
      par->UnknownExceptionCleanup();
    }
    par->last_compiled_doc = ""; // undo the damage so F4 works
    par->clearCurrentOutput();
  }
}

void TabManager::setTabName(const QString& filename, EditorInterface *edt)
{
  if (edt == nullptr) {
    edt = editor;
  }

  QString fname;
  if (filename.isEmpty()) {
    edt->filepath.clear();
    fname = _("Untitled.scad");
    tabWidget->setTabText(tabWidget->indexOf(edt), fname);
    tabWidget->setTabToolTip(tabWidget->indexOf(edt), fname);
  } else {
    QFileInfo fileinfo(filename);
    edt->filepath = fileinfo.absoluteFilePath();
    fname = fileinfo.fileName();
    tabWidget->setTabText(tabWidget->indexOf(edt), QString(fname).replace("&", "&&"));
    tabWidget->setTabToolTip(tabWidget->indexOf(edt), fileinfo.filePath());
    QDir::setCurrent(fileinfo.dir().absolutePath());
  }
  par->editorTopLevelChanged(par->editorDock->isFloating());
  par->changedTopLevelConsole(par->consoleDock->isFloating());
  par->parameterTopLevelChanged(par->parameterDock->isFloating());
  par->setWindowTitle(fname);
}

bool TabManager::refreshDocument()
{
  bool file_opened = false;
  par->setCurrentOutput();
  if (!editor->filepath.isEmpty()) {
    QFile file(editor->filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      LOG("Failed to open file %1$s: %2$s",
          editor->filepath.toLocal8Bit().constData(), file.errorString().toLocal8Bit().constData());
    } else {
      QTextStream reader(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
      reader.setCodec("UTF-8");
#endif
      auto text = reader.readAll();
      LOG("Loaded design '%1$s'.", editor->filepath.toLocal8Bit().constData());
      if (editor->toPlainText() != text) {
        editor->setPlainText(text);
        setContentRenderState(); // since last render
      }
      file_opened = true;
    }
  }
  par->setCurrentOutput();
  return file_opened;
}

bool TabManager::maybeSave(int x)
{
  auto *edt = (EditorInterface *) tabWidget->widget(x);
  if (edt->isContentModified() || edt->parameterWidget->isModified()) {
    QMessageBox box(par);
    box.setText(_("The document has been modified."));
    box.setInformativeText(_("Do you want to save your changes?"));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    box.setIcon(QMessageBox::Warning);
    box.setWindowModality(Qt::ApplicationModal);
#ifdef Q_OS_MACOS
    // Cmd-D is the standard shortcut for this button on Mac
    box.button(QMessageBox::Discard)->setShortcut(QKeySequence("Ctrl+D"));
    box.button(QMessageBox::Discard)->setShortcutEnabled(true);
#endif
    auto ret = (QMessageBox::StandardButton) box.exec();

    if (ret == QMessageBox::Save) {
      return save(edt);
    } else if (ret == QMessageBox::Cancel) {
      return false;
    }
  }
  return true;
}

/*!
 * Called for whole window close, returning false will abort the close
 * operation.
 */
bool TabManager::shouldClose()
{
  foreach(EditorInterface * edt, editorList) {
    if (!(edt->isContentModified() || edt->parameterWidget->isModified())) continue;

    QMessageBox box(par);
    box.setText(_("Some tabs have unsaved changes."));
    box.setInformativeText(_("Do you want to save all your changes?"));
    box.setStandardButtons(QMessageBox::SaveAll | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::SaveAll);
    box.setIcon(QMessageBox::Warning);
    box.setWindowModality(Qt::ApplicationModal);
#ifdef Q_OS_MACOS
    // Cmd-D is the standard shortcut for this button on Mac
    box.button(QMessageBox::Discard)->setShortcut(QKeySequence("Ctrl+D"));
    box.button(QMessageBox::Discard)->setShortcutEnabled(true);
#endif
    auto ret = (QMessageBox::StandardButton) box.exec();

    if (ret == QMessageBox::Cancel) {
      return false;
    } else if (ret == QMessageBox::Discard) {
      return true;
    } else if (ret == QMessageBox::SaveAll) {
      return saveAll();
    }
  }
  return true;
}

void TabManager::saveError(const QIODevice& file, const std::string& msg, const QString& filepath)
{
  const char *fileName = filepath.toLocal8Bit().constData();
  LOG("%1$s %2$s (%3$s)", msg.c_str(), fileName, file.errorString().toLocal8Bit().constData());

  const std::string dialogFormatStr = msg + "\n\"%1\"\n(%2)";
  const QString dialogFormat(dialogFormatStr.c_str());
  QMessageBox::warning(par, par->windowTitle(), dialogFormat.arg(filepath).arg(file.errorString()));
}

/*!
 * Save current document.
 * Should _always_ write to disk, since this is called by SaveAs - i.e. don't
 * try to be smart and check for document modification here.
 */
bool TabManager::save(EditorInterface *edt)
{
  assert(edt != nullptr);

  if (edt->filepath.isEmpty()) {
    return saveAs(edt);
  } else {
    return save(edt, edt->filepath);
  }
}

bool TabManager::save(EditorInterface *edt, const QString& path)
{
  par->setCurrentOutput();

  // If available (>= Qt 5.1), use QSaveFile to ensure the file is not
  // destroyed if the device is full. Unfortunately this is not working
  // as advertised (at least in Qt 5.3) as it does not detect the device
  // full properly and happily commits a 0 byte file.
  // Checking the QTextStream status flag after flush() seems to catch
  // this condition.
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    saveError(file, _("Failed to open file for writing"), path);
    return false;
  }

  QTextStream writer(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  writer.setCodec("UTF-8");
#endif
  writer << edt->toPlainText();
  writer.flush();
  bool saveOk = writer.status() == QTextStream::Ok;
  if (saveOk) {
    saveOk = file.commit();
  } else {
    file.cancelWriting();
  }
  if (saveOk) {
    LOG("Saved design '%1$s'.", path.toLocal8Bit().constData());
    edt->parameterWidget->saveFile(path);
    edt->setContentModified(false);
    edt->parameterWidget->setModified(false);
    par->updateRecentFiles(path);
  } else {
    saveError(file, _("Error saving design"), path);
  }
  return saveOk;
}

bool TabManager::saveAs(EditorInterface *edt)
{
  assert(edt != nullptr);

  const auto dir = edt->filepath.isEmpty() ? _("Untitled.scad") : edt->filepath;
  auto filename = QFileDialog::getSaveFileName(par, _("Save File"), dir, _("OpenSCAD Designs (*.scad)"));
  if (filename.isEmpty()) {
    return false;
  }

  if (QFileInfo(filename).suffix().isEmpty()) {
    filename.append(".scad");

    // Manual overwrite check since Qt doesn't do it, when using the
    // defaultSuffix property
    const QFileInfo info(filename);
    if (info.exists()) {
      const auto text = QString(_("%1 already exists.\nDo you want to replace it?")).arg(info.fileName());
      if (QMessageBox::warning(par, par->windowTitle(), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return false;
      }
    }
  }

  bool saveOk = save(edt, filename);
  if (saveOk) {
    setTabName(filename, edt);
  }
  return saveOk;
}

bool TabManager::saveACopy(EditorInterface *edt)
{
  assert(edt != nullptr);

  const auto dir = edt->filepath.isEmpty() ? _("Untitled.scad") : edt->filepath;
  auto filename = QFileDialog::getSaveFileName(par, _("Save a Copy"), dir, _("OpenSCAD Designs (*.scad)"));
  if (filename.isEmpty()) {
    return false;
  }

  if (QFileInfo(filename).suffix().isEmpty()) {
    filename.append(".scad");
  }

  return save(edt, filename);
}

bool TabManager::saveAll()
{
  foreach(EditorInterface * edt, editorList) {
    if (edt->isContentModified() || edt->parameterWidget->isModified()) {
      if (!save(edt)) {
        return false;
      }
    }
  }
  return true;
}
