/*
 *  Copyright (C) 2018 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Merger.h"

#include "core/Clock.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Metadata.h"

static const Group::MergeMode ModeDefault = static_cast<Group::MergeMode>(-1);

Merger::Merger(const Database* sourceDb, Database* targetDb)
    : m_mode(ModeDefault)
{
    if (!sourceDb || !targetDb) {
        Q_ASSERT(sourceDb && targetDb);
        return;
    }

    m_context = MergeContext{
        sourceDb, targetDb, sourceDb->rootGroup(), targetDb->rootGroup(), sourceDb->rootGroup(), targetDb->rootGroup()};
}

Merger::Merger(const Group* sourceGroup, Group* targetGroup)
    : m_mode(ModeDefault)
{
    if (!sourceGroup || !targetGroup) {
        Q_ASSERT(sourceGroup && targetGroup);
        return;
    }

    m_context = MergeContext{sourceGroup->database(),
                             targetGroup->database(),
                             sourceGroup->database()->rootGroup(),
                             targetGroup->database()->rootGroup(),
                             sourceGroup,
                             targetGroup};
}

void Merger::setForcedMergeMode(Group::MergeMode mode)
{
    m_mode = mode;
}

void Merger::resetForcedMergeMode()
{
    m_mode = ModeDefault;
}

bool Merger::merge()
{
    // Order of merge steps is important - it is possible that we
    // create some items before deleting them afterwards
    ChangeList changes;
    changes << mergeGroup(m_context);
    changes << mergeDeletions(m_context);
    changes << mergeMetadata(m_context);

    // qDebug("Merged %s", qPrintable(changes.join("\n\t")));

    // At this point we have a list of changes we may want to show the user
    if (!changes.isEmpty()) {
        m_context.m_targetDb->markAsModified();
        return true;
    }
    return false;
}

Merger::ChangeList Merger::mergeGroup(const MergeContext& context)
{
    ChangeList changes;
    // merge entries
    const QList<Entry*> sourceEntries = context.m_sourceGroup->entries();
    for (Entry* sourceEntry : sourceEntries) {
        Entry* targetEntry = context.m_targetRootGroup->findEntryByUuid(sourceEntry->uuid());
        if (!targetEntry) {
            changes << tr("Creating missing %1 [%2]").arg(sourceEntry->title()).arg(sourceEntry->uuid().toHex());
            // This entry does not exist at all. Create it.
            targetEntry = sourceEntry->clone(Entry::CloneIncludeHistory);
            moveEntry(targetEntry, context.m_targetGroup);
        } else {
            // Entry is already present in the database. Update it.
            bool locationChanged =
                targetEntry->timeInfo().locationChanged() < sourceEntry->timeInfo().locationChanged();
            if (locationChanged && targetEntry->group() != context.m_targetGroup) {
                changes << tr("Relocating %1 [%2]").arg(sourceEntry->title()).arg(sourceEntry->uuid().toHex());
                moveEntry(targetEntry, context.m_targetGroup);
            }
            changes << resolveEntryConflict(context, sourceEntry, targetEntry);
        }
    }

    // merge groups recursively
    const QList<Group*> sourceChildGroups = context.m_sourceGroup->children();
    for (Group* sourceChildGroup : sourceChildGroups) {
        Group* targetChildGroup = context.m_targetRootGroup->findGroupByUuid(sourceChildGroup->uuid());
        if (!targetChildGroup) {
            changes << tr("Creating missing %1 [%2]").arg(sourceChildGroup->name()).arg(sourceChildGroup->uuid().toHex());
            targetChildGroup = sourceChildGroup->clone(Entry::CloneNoFlags, Group::CloneNoFlags);
            moveGroup(targetChildGroup, context.m_targetGroup);
            TimeInfo timeinfo = targetChildGroup->timeInfo();
            timeinfo.setLocationChanged(sourceChildGroup->timeInfo().locationChanged());
            targetChildGroup->setTimeInfo(timeinfo);
        } else {
            bool locationChanged =
                targetChildGroup->timeInfo().locationChanged() < sourceChildGroup->timeInfo().locationChanged();
            if (locationChanged && targetChildGroup->parent() != context.m_targetGroup) {
                changes << tr("Relocating %1 [%2]").arg(sourceChildGroup->name()).arg(sourceChildGroup->uuid().toHex());
                moveGroup(targetChildGroup, context.m_targetGroup);
                TimeInfo timeinfo = targetChildGroup->timeInfo();
                timeinfo.setLocationChanged(sourceChildGroup->timeInfo().locationChanged());
                targetChildGroup->setTimeInfo(timeinfo);
            }
            changes << resolveGroupConflict(context, sourceChildGroup, targetChildGroup);
        }
        MergeContext subcontext{context.m_sourceDb,
                                context.m_targetDb,
                                context.m_sourceRootGroup,
                                context.m_targetRootGroup,
                                sourceChildGroup,
                                targetChildGroup};
        changes << mergeGroup(subcontext);
    }
    return changes;
}

Merger::ChangeList
Merger::resolveGroupConflict(const MergeContext& context, const Group* sourceChildGroup, Group* targetChildGroup)
{
    Q_UNUSED(context);
    ChangeList changes;

    const QDateTime timeExisting = targetChildGroup->timeInfo().lastModificationTime();
    const QDateTime timeOther = sourceChildGroup->timeInfo().lastModificationTime();

    // only if the other group is newer, update the existing one.
    if (timeExisting < timeOther) {
        changes << tr("Overwriting %1 [%2]").arg(sourceChildGroup->name()).arg(sourceChildGroup->uuid().toHex());
        targetChildGroup->setName(sourceChildGroup->name());
        targetChildGroup->setNotes(sourceChildGroup->notes());
        if (sourceChildGroup->iconNumber() == 0) {
            targetChildGroup->setIcon(sourceChildGroup->iconUuid());
        } else {
            targetChildGroup->setIcon(sourceChildGroup->iconNumber());
        }
        targetChildGroup->setExpiryTime(sourceChildGroup->timeInfo().expiryTime());
        // TODO HNH: Since we are updating our own group from the source group, I think we should update the timestamp
    }
    return changes;
}

bool Merger::markOlderEntry(Entry* entry)
{
    entry->attributes()->set(
        "merged", tr("older entry merged from database \"%1\"").arg(entry->group()->database()->metadata()->name()));
    return true;
}

void Merger::moveEntry(Entry* entry, Group* targetGroup)
{
    Q_ASSERT(entry);
    Group* sourceGroup = entry->group();
    if (sourceGroup == targetGroup) {
        return;
    }
    const bool sourceGroupUpdateTimeInfo = sourceGroup ? sourceGroup->canUpdateTimeinfo() : false;
    if (sourceGroup) {
        sourceGroup->setUpdateTimeinfo(false);
    }
    const bool targetGroupUpdateTimeInfo = targetGroup ? targetGroup->canUpdateTimeinfo() : false;
    if (targetGroup) {
        targetGroup->setUpdateTimeinfo(false);
    }
    const bool entryUpdateTimeInfo = entry->canUpdateTimeinfo();
    entry->setUpdateTimeinfo(false);

    entry->setGroup(targetGroup);

    entry->setUpdateTimeinfo(entryUpdateTimeInfo);
    if (targetGroup) {
        targetGroup->setUpdateTimeinfo(targetGroupUpdateTimeInfo);
    }
    if (sourceGroup) {
        sourceGroup->setUpdateTimeinfo(sourceGroupUpdateTimeInfo);
    }
}

void Merger::moveGroup(Group* group, Group* targetGroup)
{
    Q_ASSERT(group);
    Group* sourceGroup = group->parentGroup();
    if (sourceGroup == targetGroup) {
        return;
    }
    const bool sourceGroupUpdateTimeInfo = sourceGroup ? sourceGroup->canUpdateTimeinfo() : false;
    if (sourceGroup) {
        sourceGroup->setUpdateTimeinfo(false);
    }
    const bool targetGroupUpdateTimeInfo = targetGroup ? targetGroup->canUpdateTimeinfo() : false;
    if (targetGroup) {
        targetGroup->setUpdateTimeinfo(false);
    }
    const bool groupUpdateTimeInfo = group->canUpdateTimeinfo();
    group->setUpdateTimeinfo(false);

    group->setParent(targetGroup);

    group->setUpdateTimeinfo(groupUpdateTimeInfo);
    if (targetGroup) {
        targetGroup->setUpdateTimeinfo(targetGroupUpdateTimeInfo);
    }
    if (sourceGroup) {
        sourceGroup->setUpdateTimeinfo(sourceGroupUpdateTimeInfo);
    }
}

void Merger::eraseEntry(Entry* entry)
{
    Database* database = entry->database();
    // most simple method to remove an item from DeletedObjects :(
    const QList<DeletedObject> deletions = database->deletedObjects();
    Group* parentGroup = entry->group();
    const bool groupUpdateTimeInfo = parentGroup ? parentGroup->canUpdateTimeinfo() : false;
    if (parentGroup) {
        parentGroup->setUpdateTimeinfo(false);
    }
    delete entry;
    if (parentGroup) {
        parentGroup->setUpdateTimeinfo(groupUpdateTimeInfo);
    }
    database->setDeletedObjects(deletions);
}

void Merger::eraseGroup(Group* group)
{
    Database* database = group->database();
    // most simple method to remove an item from DeletedObjects :(
    const QList<DeletedObject> deletions = database->deletedObjects();
    Group* parentGroup = group->parentGroup();
    const bool groupUpdateTimeInfo = parentGroup ? parentGroup->canUpdateTimeinfo() : false;
    if (parentGroup) {
        parentGroup->setUpdateTimeinfo(false);
    }
    delete group;
    if (parentGroup) {
        parentGroup->setUpdateTimeinfo(groupUpdateTimeInfo);
    }
    database->setDeletedObjects(deletions);
}

Merger::ChangeList
Merger::resolveEntryConflict(const MergeContext& context, const Entry* sourceEntry, Entry* targetEntry)
{
    ChangeList changes;
    // We need to cut off the milliseconds since the persistent format only supports times down to seconds
    // so when we import data from a remote source, it may represent the (or even some msec newer) data
    // which may be discarded due to higher runtime precision
    const auto timeTarget = Clock::serialized(targetEntry->timeInfo().lastModificationTime());
    const auto timeSource = Clock::serialized(sourceEntry->timeInfo().lastModificationTime());

    Group::MergeMode mergeMode = m_mode == ModeDefault ? context.m_targetGroup->mergeMode() : m_mode;

    switch (mergeMode) {
    case Group::KeepBoth:
        // if one entry is newer, create a clone and add it to the group
        if (timeTarget > timeSource) {
            Entry* clonedEntry = sourceEntry->clone(Entry::CloneNewUuid | Entry::CloneIncludeHistory);
            moveEntry(clonedEntry, context.m_targetGroup);
            markOlderEntry(clonedEntry);
            changes << tr("Adding backup for older source %1 [%2]")
                           .arg(sourceEntry->title())
                           .arg(sourceEntry->uuid().toHex());
        } else if (timeTarget < timeSource) {
            Entry* clonedEntry = sourceEntry->clone(Entry::CloneNewUuid | Entry::CloneIncludeHistory);
            moveEntry(clonedEntry, context.m_targetGroup);
            markOlderEntry(targetEntry);
            changes << tr("Adding backup for older target %1 [%2]")
                           .arg(targetEntry->title())
                           .arg(targetEntry->uuid().toHex());
        }
        break;

    case Group::KeepNewer:
        if (timeTarget < timeSource) {
            // only if other entry is newer, replace existing one
            Entry* clonedEntry = sourceEntry->clone(Entry::CloneIncludeHistory);
            Group* currentGroup = targetEntry->group();
            qDebug("Updating entry %s.", qPrintable(targetEntry->title()));
            moveEntry(clonedEntry, currentGroup);
            eraseEntry(targetEntry);
            changes << tr("Overwriting %1 [%2]").arg(clonedEntry->title()).arg(clonedEntry->uuid().toHex());
        }
        break;

    case Group::KeepExisting:
        break;

    case Group::Synchronize:
        if (timeTarget < timeSource) {
            Group* currentGroup = targetEntry->group();
            Entry* clonedEntry = sourceEntry->clone(Entry::CloneIncludeHistory);
            qDebug("Merge %s/%s with alien on top under %s",
                   qPrintable(targetEntry->title()),
                   qPrintable(sourceEntry->title()),
                   qPrintable(currentGroup->name()));
            changes << tr("Synchronizing from newer source %1 [%2]")
                           .arg(targetEntry->title())
                           .arg(targetEntry->uuid().toHex());
            moveEntry(clonedEntry, currentGroup);
            mergeHistory(targetEntry, clonedEntry);
            eraseEntry(targetEntry);
        } else {
            qDebug("Merge %s/%s with local on top/under %s",
                   qPrintable(targetEntry->title()),
                   qPrintable(sourceEntry->title()),
                   qPrintable(targetEntry->group()->name()));
            const bool changed = mergeHistory(sourceEntry, targetEntry);
            if (changed) {
                changes << tr("Synchronizing from older source %1 [%2]")
                               .arg(targetEntry->title())
                               .arg(targetEntry->uuid().toHex());
            }
        }
        break;

    default:
        // do nothing
        break;
    }
    return changes;
}

bool Merger::mergeHistory(const Entry* sourceEntry, Entry* targetEntry)
{
    const auto targetHistoryItems = targetEntry->historyItems();
    const auto sourceHistoryItems = sourceEntry->historyItems();

    QMap<QDateTime, Entry*> merged;
    for (Entry* historyItem : targetHistoryItems) {
        const QDateTime modificationTime = Clock::serialized(historyItem->timeInfo().lastModificationTime());
        Q_ASSERT(!merged.contains(modificationTime)
                 || merged[modificationTime]->equals(historyItem, CompareItemIgnoreMilliseconds));
        merged[modificationTime] = historyItem->clone(Entry::CloneNoFlags);
    }
    for (Entry* historyItem : sourceHistoryItems) {
        // Items with same modification-time changes will be regarded as same (like KeePass2)
        const QDateTime modificationTime = Clock::serialized(historyItem->timeInfo().lastModificationTime());
        Q_ASSERT(!merged.contains(modificationTime)
                 || merged[modificationTime]->equals(historyItem, CompareItemIgnoreMilliseconds));
        if (!merged.contains(modificationTime)) {
            merged[modificationTime] = historyItem->clone(Entry::CloneNoFlags);
        }
    }

    const QDateTime targetModificationTime = Clock::serialized(targetEntry->timeInfo().lastModificationTime());
    const QDateTime sourceModificationTime = Clock::serialized(sourceEntry->timeInfo().lastModificationTime());
    Q_ASSERT(targetModificationTime != sourceModificationTime
             || targetEntry->equals(
                    sourceEntry, CompareItemIgnoreMilliseconds | CompareItemIgnoreHistory | CompareItemIgnoreLocation));

    if (targetModificationTime < sourceModificationTime && !merged.contains(targetModificationTime)) {
        merged[targetModificationTime] = targetEntry->clone(Entry::CloneNoFlags);
    } else if (targetModificationTime > sourceModificationTime && !merged.contains(sourceModificationTime)) {
        merged[sourceModificationTime] = sourceEntry->clone(Entry::CloneNoFlags);
    }

    bool changed = false;
    const int maxItems = targetEntry->database()->metadata()->historyMaxItems();
    const auto updatedHistoryItems = merged.values();
    for (int i = 0; i < maxItems; ++i) {
        const Entry* oldEntry = targetHistoryItems.value(targetHistoryItems.count() - i);
        const Entry* newEntry = updatedHistoryItems.value(updatedHistoryItems.count() - i);
        if (!oldEntry && !newEntry) {
            continue;
        }
        if (oldEntry && newEntry && oldEntry->equals(newEntry, CompareItemIgnoreMilliseconds)) {
            continue;
        }
        changed = true;
        break;
    }
    if (!changed) {
        qDeleteAll(updatedHistoryItems);
        return false;
    }
    // We need to prevent any modification to the database since every change should be tracked either
    // in a clone history item or in the Entry itself
    const TimeInfo timeInfo = targetEntry->timeInfo();
    const bool blockedSignals = targetEntry->blockSignals(true);
    bool updateTimeInfo = targetEntry->canUpdateTimeinfo();
    targetEntry->setUpdateTimeinfo(false);
    targetEntry->removeHistoryItems(targetHistoryItems);
    for (Entry* historyItem : merged.values()) {
        Q_ASSERT(!historyItem->parent());
        targetEntry->addHistoryItem(historyItem);
    }
    targetEntry->truncateHistory();
    targetEntry->blockSignals(blockedSignals);
    targetEntry->setUpdateTimeinfo(updateTimeInfo);
    Q_ASSERT(timeInfo == targetEntry->timeInfo());
    Q_UNUSED(timeInfo);
    return true;
}

Merger::ChangeList Merger::mergeDeletions(const MergeContext& context)
{
    ChangeList changes;
    const auto targetDeletions = context.m_targetDb->deletedObjects();
    const auto sourceDeletions = context.m_sourceDb->deletedObjects();

    QList<DeletedObject> deletions;
    QMap<Uuid, DeletedObject> mergedDeletions;
    QList<Entry*> entries;
    QList<Group*> groups;
    for (const auto& object : (targetDeletions + sourceDeletions)) {
        if (!mergedDeletions.contains(object.uuid)) {
            mergedDeletions[object.uuid] = object;

            auto* entry = context.m_targetRootGroup->findEntryByUuid(object.uuid);
            if (entry) {
                entries << entry;
                continue;
            }
            auto* group = context.m_targetRootGroup->findGroupByUuid(object.uuid);
            if (group) {
                groups << group;
                continue;
            }
            deletions << object;
            continue;
        }
        if (mergedDeletions[object.uuid].deletionTime > object.deletionTime) {
            mergedDeletions[object.uuid] = object;
        }
    }

    while (!entries.isEmpty()) {
        auto* entry = entries.takeFirst();
        const auto& object = mergedDeletions[entry->uuid()];
        if (entry->timeInfo().lastModificationTime() > object.deletionTime) {
            // keep deleted entry since it was changed after deletion date
            continue;
        }
        deletions << object;
        if (entry->group()) {
            changes << tr("Deleting child %1 [%2]").arg(entry->title()).arg(entry->uuid().toHex());
        } else {
            changes << tr("Deleting orphan %1 [%2]").arg(entry->title()).arg(entry->uuid().toHex());
        }
        // Entry is inserted into deletedObjects after deletions are processed
        eraseEntry(entry);
    }

    while (!groups.isEmpty()) {
        auto* group = groups.takeFirst();
        if (!(group->children().toSet() & groups.toSet()).isEmpty()) {
            // we need to finish all children before we are able to determine if the group can be removed
            groups << group;
            continue;
        }
        const auto& object = mergedDeletions[group->uuid()];
        if (group->timeInfo().lastModificationTime() > object.deletionTime) {
            // keep deleted group since it was changed after deletion date
            continue;
        }
        if (!group->entriesRecursive(false).isEmpty() || !group->groupsRecursive(false).isEmpty()) {
            // keep deleted group since it contains undeleted content
            continue;
        }
        deletions << object;
        if (group->parentGroup()) {
            changes << tr("Deleting child %1 [%2]").arg(group->name()).arg(group->uuid().toHex());
        } else {
            changes << tr("Deleting orphan %1 [%2]").arg(group->name()).arg(group->uuid().toHex());
        }
        eraseGroup(group);
    }
    // Put every deletion to the earliest date of deletion
    if (deletions != context.m_targetDb->deletedObjects()) {
        changes << tr("Changed deleted objects");
    }
    context.m_targetDb->setDeletedObjects(deletions);
    return changes;
}

Merger::ChangeList Merger::mergeMetadata(const MergeContext& context)
{
    // TODO HNH: missing handling of recycle bin, names, templates for groups and entries,
    //           public data (entries of newer dict override keys of older dict - ignoring
    //           their own age - it is enough if one entry of the whole dict is newer) => possible lost update
    // TODO HNH: CustomData is merged with entries of the new customData overwrite entries
    //           of the older CustomData - the dict with the newest entry is considered
    //           newer regardless of the age of the other entries => possible lost update
    ChangeList changes;
    auto* sourceMetadata = context.m_sourceDb->metadata();
    auto* targetMetadata = context.m_targetDb->metadata();

    for (Uuid customIconId : sourceMetadata->customIcons().keys()) {
        QImage customIcon = sourceMetadata->customIcon(customIconId);
        if (!targetMetadata->containsCustomIcon(customIconId)) {
            targetMetadata->addCustomIcon(customIconId, customIcon);
            changes << tr("Adding missing icon %1").arg(customIconId.toHex());
        }
    }
    return changes;
}
