/*
For general Scribus (>=1.3.2) copyright and licensing information please refer
to the COPYING file provided with the program. Following this notice may exist
a copyright and/or license notice that predates the release of Scribus 1.3.2
for which a new license (GPL+exception) is in place.
*/
/***************************************************************************
 *   Copyright (C) 2005 by Riku Leino                                      *
 *   riku@scribus.info                                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.             *
 ***************************************************************************/

#include <set>
#include <map>

#include "bulkadder.h"

namespace {
void findName(PageItem *item, std::set<QString> &names,
              std::map<PageItem::ItemType, int> &lastSuccess) {
  auto type = item->itemType();
  QString baseName = item->nameFromType(type);

  int nameNum = lastSuccess[type];
  QString tmp;
  tmp.setNum(nameNum);
  tmp.prepend(baseName);
  while (names.count(tmp) != 0) {
    ++nameNum;
    tmp.setNum(nameNum);
    tmp.prepend(baseName);
  }
  lastSuccess[type] = nameNum;
  names.insert(tmp);
  item->setSafeItemName(tmp);
}

std::set<QString> gatherNames(ScribusDoc *doc) {
  std::set<QString> names;
  std::vector<PageItem *> groups;
  groups.reserve(32);

  // Process root elements of the doc and remember groups
  for (PageItem *item : *doc->Items) {
    names.insert(item->itemName());
    if (item->isGroup())
      groups.push_back(item);
  }

  // Process groups
  while (!groups.empty()) {
    PageItem *item = groups.back();
    groups.pop_back();
    for (PageItem *item : item->groupItemList) {
      names.insert(item->itemName());
      if (item->isGroup())
        groups.push_back(item);
    }
  }
  return names;
}
} // namespace

int BulkAdder::itemAdd(const PageItem::ItemType itemType,
                       const PageItem::ItemFrameType frameType, double x,
                       double y, double b, double h, double w,
                       const QString &fill, const QString &outline,
                       PageItem::ItemKind itemKind) {
  int id = m_doc->itemAddDeferred(itemType, frameType, x, y, b, h, w, fill,
                                  outline, itemKind);
  m_pending.push_back(m_doc->Items->at(id));
  return id;
}


PageItem* BulkAdder::groupObjectsSelection(Selection* customSelection)
{
  auto group = m_doc->groupObjectsSelectionDeferred(customSelection);
  m_pending.push_back(group);
  return group;
}

void BulkAdder::process() {
  if (m_pending.empty()) {
    return;
  }

  auto names = gatherNames(m_doc);
  std::map<PageItem::ItemType, int> lastSuccess;
  for (auto item : m_pending) {
    findName(item, names, lastSuccess);
  }
  m_pending.clear();
}
