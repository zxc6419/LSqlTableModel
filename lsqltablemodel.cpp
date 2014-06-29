#include "lsqltablemodel.h"
#include <QDebug>
#include <QSqlError>

/*!
    \class LSqlTableModel
    \brief The alternative to standard QSqlTable model class that provides an editable data model
    for a single database table.

    The main idea of LSqlTableModel is to avoid full table reselection after rows deletion
    and creation. There isn't any caching for \c removeRow() operation though row modification and 
    new rows creation is cached until \c submit or \c revert methods called.
    
*/

LSqlTableModel::LSqlTableModel(QObject *parent, QSqlDatabase db) :
    QAbstractTableModel(parent)
{
  _db = db.isValid() ? db : QSqlDatabase::database();
  _query = QSqlQuery(_db);
}

/*!
    Sets the database table the model should be populated with
*/
bool LSqlTableModel::setTable(QString tableName)
{
  _tableName = tableName;
  _patternRec = _db.record(_tableName);
  _primaryIndex = _db.primaryIndex(_tableName);
  return true;
}

/*!
    Populates the model with table data
*/
bool LSqlTableModel::reloadTable()
{
  QString sql = "select * from %1";
  if (!execQuery(sql.arg(_tableName))){
    return false;
  }

  //Заполнение индекса и карты записей
  while (_query.next()){
    _recIndex.append(_query.value("ID").toInt());
    _recMap.insert(_query.value("ID").toInt(), LSqlRecord(_query.record()));
  }
  return true;
}

/*!
    Trys to submit all cached (unsaved) changes to the database table.
    Returns \c true if all cached changes were successfully submitted.
*/
bool LSqlTableModel::submitAll()
{
  //Если не было изменений, сразу выход
  if (!_modified)
    return true;

  CacheMap::Iterator it;
  bool result = true;
  for (it = _recMap.begin(); it != _recMap.end(); ++it){
    result &= submitRecord(it.value());
  }
  _modified = !result;
  return result;
}

/*!
    Returns number of rows that the model contains
*/
int LSqlTableModel::rowCount(const QModelIndex &parent) const
{
  return _recMap.count();
}

/*!
    Returns number of columns tha the model contains
*/
int LSqlTableModel::columnCount(const QModelIndex &parent) const
{
  return _patternRec.count();
}

/*!
    Overriden virtual method that returns the model data for roles \c Qt::DisplayRole and
    \c Qt::EditRole.
*/
QVariant LSqlTableModel::data(const QModelIndex &index, int role) const
{
  if (!index.isValid())
    return QVariant();
  switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
    {
      LSqlRecord rec = _recMap[_recIndex.at(index.row())];
      return rec.value(index.column());
    }
    default:
      return QVariant();
  }
}

/*!
    Overriden virtual method that used to save the data to the model.
    Rows modified by this method would be marked with cache action \c LSqlRecord::Update 
    unless they are already marked with cache action \c LSqlRecord::Insert.
*/
bool LSqlTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
  qDebug() << "old val: " << data(index) << "new data: " << value;
  if (role == Qt::EditRole && data(index) != value){
    LSqlRecord &rec = _recMap[_recIndex.at(index.row())];
    rec.setValue(index.column(), value);
    emit beforeUpdate(rec);
    setCacheAction(rec, LSqlRecord::Update);
  }
  return true;
}

/*!
    Overriden virtual method that manages properties of model items
    (such as \c Qt::ItemIsEnabled Qt::ItemIsSelectable  Qt::ItemIsEditable)
*/
Qt::ItemFlags LSqlTableModel::flags(const QModelIndex &index) const
{
  if (!index.isValid())
    return 0;
  //Первичный ключ редактировать нельзя
  if (_primaryIndex.indexOf(_patternRec.fieldName(index.column())) >= 0){
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  }
  return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

/*!
    Overriden virtual method that manages header item data of a view widget
*/
QVariant LSqlTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (role == Qt::DisplayRole){
    if (orientation == Qt::Vertical){
      return section;
    }
    else {
      return _patternRec.fieldName(section);
    }
  }
  return QVariant();
}

/*!
    Inserts new row into the model. Supports adding of only one row at a time.
    Primary key value of a new record filled through \c nextId() call. Other record
    fields can be filled with initial values by handling signal \c beforeInsert(QSqlRecord)
*/
bool LSqlTableModel::insertRows(int row, int count, const QModelIndex &parent)
{
  //Только для таблиц и по одной строке
  if (parent.isValid() || count > 1)
    return false;

  //Пробуем получить ID для новой записи
  int newId = nextId();
  if (newId < 0)
    return false;

  LSqlRecord newRec(_patternRec);
  newRec.setValue("ID", QVariant(newId));

  //сигнал для инициализации новой строки (записи)
  emit beforeInsert(newRec);

  setCacheAction(newRec, LSqlRecord::Insert);

  beginInsertRows(parent, row, row + count - 1);
  _recIndex.insert(row, newId);
  _recMap.insert(newId, newRec);
  endInsertRows();
  return true;
}

/*!
    Removes row from the model and deletes the corresponding record from
    the database table. The operation isn't cachable unlike QSqlTableModel.
*/
bool LSqlTableModel::removeRows(int row, int count, const QModelIndex &parent)
{
  //Только для таблиц и по одной строке
  if (parent.isValid() || count > 1)
    return false;

  QSqlRecord delRec(_primaryIndex);
  delRec.setValue("ID", (int)_recIndex.at(row));

  if (!deleteRowInTable(delRec))
    return false;

  beginRemoveRows(parent, row, row + count - 1);
  _recMap.remove(_recIndex[row]);
  _recIndex.removeAt(row);
  endRemoveRows();
  return true;
}

/*!
    Sets cache operation mark for the model row.
*/
void LSqlTableModel::setCacheAction(LSqlRecord &rec, LSqlRecord::CacheAction action)
{
  //Вставленная запись остается вставленной, даже если ее редактировать
  if (rec.cacheAction() == LSqlRecord::Insert && action == LSqlRecord::Update)
    return;

  rec.setCacheAction(action);
  if (action != LSqlRecord::None && !_modified)
    _modified = true;
}

/*!
    Submit cached changes to the corresponding database table record
*/
bool LSqlTableModel::submitRecord(LSqlRecord &rec)
{
  //Запись не редактировалась
  if (rec.cacheAction() == LSqlRecord::None)
    return true;

  bool result = true;
  if (rec.cacheAction() == LSqlRecord::Update){
    result = updateRowInTable(rec);
  }
  else if (rec.cacheAction() == LSqlRecord::Insert) {
    result = insertRowInTable(rec);
  }
  if (result){
    setCacheAction(rec, LSqlRecord::None);
  }
  else {
    qDebug() << "Error while submitting record: " << rec;
  }
  return result;
}

/*!
  Revert all cached changes to the model data.
  Note: Remove operation connot be cached unlike QSqlTableModel.
*/
bool LSqlTableModel::revertAll()
{
  //Если не было изменений, сразу выход
  if (!_modified)
    return true;

  bool result = true;
  int row = 0;
  while (row < _recIndex.count()){
    switch(_recMap[_recIndex[row]].cacheAction()){
    case LSqlRecord::Insert:
      beginRemoveRows(QModelIndex(), row, row);
      _recMap.remove(_recIndex[row]);
      _recIndex.removeAt(row);
      endRemoveRows();
      break;
    case LSqlRecord::Update:
      result &= reloadRow(row);
      row++;
      break;
    default:
      row++;
    }
  }
  _modified = !result;

  return result;
}

bool LSqlTableModel::reloadRow(int row)
{
  LSqlRecord& rec = _recMap[_recIndex[row]];
  if (rec.cacheAction() != LSqlRecord::Update)
    return true;
  bool result = selectRowInTable(rec);
  if (result){
    rec.setCacheAction(LSqlRecord::None);
    //Сигнал представлению, что надо обновить данные строки
    emit dataChanged(createIndex(row, 0), createIndex(row, columnCount() - 1));
  }
  return result;
}

bool LSqlTableModel::selectRowInTable(QSqlRecord &values)
{
  QSqlRecord whereValues = primaryValues(values);
  QString stmt = _db.driver()->sqlStatement(QSqlDriver::SelectStatement, _tableName,
                                                   values, false);
  QString where = _db.driver()->sqlStatement(QSqlDriver::WhereStatement, _tableName,
                                                     whereValues, false);
  QString sql = Sql::concat(stmt, where);
  bool result = execQuery(sql);
  //Если запрос выполнен и есть результат
  if (result && _query.next()){
    QSqlRecord resRec = _query.record();
    for(int i = 0; i < resRec.count(); i++){
      values.setValue(i, resRec.value(i));
    }
    return true;
  }
  return false;
}

bool LSqlTableModel::updateRowInTable(const QSqlRecord &values)
{
    QSqlRecord rec(values);    
    QSqlRecord whereValues = primaryValues(values);

    QString stmt = _db.driver()->sqlStatement(QSqlDriver::UpdateStatement, _tableName,
                                                     rec, false);
    QString where = _db.driver()->sqlStatement(QSqlDriver::WhereStatement, _tableName,
                                                       whereValues, false);
    QString sql = Sql::concat(stmt, where);
    return execQuery(sql);
}

bool LSqlTableModel::insertRowInTable(const QSqlRecord &values)
{
  QString stmt = _db.driver()->sqlStatement(QSqlDriver::InsertStatement, _tableName,
                                                   values, false);
  return execQuery(stmt);
}

bool LSqlTableModel::deleteRowInTable(const QSqlRecord &values)
{
  QSqlRecord rec(values);
  QString stmt = _db.driver()->sqlStatement(QSqlDriver::DeleteStatement, _tableName,
                                            rec, false);
  QString where = _db.driver()->sqlStatement(QSqlDriver::WhereStatement, _tableName,
                                                     values, false);
  QString sql = Sql::concat(stmt, where);
  return execQuery(sql);
}

QSqlRecord LSqlTableModel::primaryValues(QSqlRecord rec) const
{
  QSqlRecord r(_primaryIndex);
  for (int i=0; i<r.count(); i++){
    r.setValue(i, rec.value(r.fieldName(i)));
  }
  return r;
}

/*!
    Returns a subsequent value of the database generator (sequence).
    Name of the generator can be set by method \c setSequenceName.
*/
int LSqlTableModel::nextId()
{
  if (_sequenceName.isEmpty()){
    qDebug() << "No sequence specified for table " << _tableName;
    return -1;
  }

  QString sql = "select GEN_ID(%1, 1) from rdb$database";
  if (!execQuery(sql.arg(_sequenceName))){
      qDebug() << "Error handling sequence: " << _sequenceName;
      return -1;
  }
  _query.next();
  return _query.value(0).toInt();
}

/*!
    A wrapper function for all sql-queries.
    Sends all executed sql-queries to qDebug() in case
    of success and error messages in case of failure.
*/
bool LSqlTableModel::execQuery(const QString &sql)
{
  bool result = _query.exec(sql);
  qDebug() << "Execute query: " << sql;
  if (!result){
    qDebug() << "Error: " << _query.lastError().databaseText();
  }
  return result;
}


LSqlRecord::LSqlRecord(): QSqlRecord()
{
  _cacheAction = LSqlRecord::None;
}

LSqlRecord::LSqlRecord(const QSqlRecord &rec): QSqlRecord(rec)
{
  _cacheAction = LSqlRecord::None;
}