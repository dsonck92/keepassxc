/*
 *  Copyright (C) 2010 Felix Geyer <debfx@fobos.de>
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

#include "TimeInfo.h"

#include "core/Clock.h"
#include "core/Tools.h"

TimeInfo::TimeInfo()
    : m_expires(false)
    , m_usageCount(0)
{
    QDateTime now = Clock::currentDateTimeUtc();
    m_lastModificationTime = now;
    m_creationTime = now;
    m_lastAccessTime = now;
    m_expiryTime = now;
    m_locationChanged = now;
}

QDateTime TimeInfo::lastModificationTime(Precision precision) const
{
    return precision == High ? m_lastModificationTime : Clock::normalize(m_lastModificationTime);
}

QDateTime TimeInfo::creationTime(Precision precision) const
{
    return precision == High ? m_creationTime : Clock::normalize(m_creationTime);
}

QDateTime TimeInfo::lastAccessTime(Precision precision) const
{
    return precision == High ? m_lastAccessTime : Clock::normalize(m_lastAccessTime);
}

QDateTime TimeInfo::expiryTime(Precision precision) const
{
    return precision == High ? m_expiryTime : Clock::normalize(m_expiryTime);
}

bool TimeInfo::expires() const
{
    return m_expires;
}

int TimeInfo::usageCount() const
{
    return m_usageCount;
}

QDateTime TimeInfo::locationChanged(Precision precision) const
{
    return precision == High ? m_locationChanged : Clock::normalize(m_locationChanged);
}

void TimeInfo::setLastModificationTime(const QDateTime& dateTime)
{
    Q_ASSERT(dateTime.timeSpec() == Qt::UTC);
    m_lastModificationTime = dateTime;
}

void TimeInfo::setCreationTime(const QDateTime& dateTime)
{
    Q_ASSERT(dateTime.timeSpec() == Qt::UTC);
    m_creationTime = dateTime;
}

void TimeInfo::setLastAccessTime(const QDateTime& dateTime)
{
    Q_ASSERT(dateTime.timeSpec() == Qt::UTC);
    m_lastAccessTime = dateTime;
}

void TimeInfo::setExpiryTime(const QDateTime& dateTime)
{
    Q_ASSERT(dateTime.timeSpec() == Qt::UTC);
    m_expiryTime = dateTime;
}

void TimeInfo::setExpires(bool expires)
{
    m_expires = expires;
}

void TimeInfo::setUsageCount(int count)
{
    m_usageCount = count;
}

void TimeInfo::setLocationChanged(const QDateTime& dateTime)
{
    Q_ASSERT(dateTime.timeSpec() == Qt::UTC);
    m_locationChanged = dateTime;
}

bool TimeInfo::operator==(const TimeInfo& other) const
{
    return equals(other, CompareDefault);
}

bool TimeInfo::equals(const TimeInfo& other, CompareOptions options) const
{
    if (::compare(m_lastModificationTime, other.m_lastModificationTime, options) != 0) {
        return false;
    }
    if (::compare(m_creationTime, other.m_creationTime, options) != 0) {
        return false;
    }
    if (::compare(m_lastAccessTime, other.m_lastAccessTime, options | CompareRepresentsStatistic) != 0) {
        return false;
    }
    if (::compare(m_expires, m_expiryTime, other.m_expires, other.expiryTime(), options) != 0) {
        return false;
    }
    if (::compare(m_usageCount, other.m_usageCount, options | CompareRepresentsStatistic) != 0) {
        return false;
    }
    if (::compare(m_locationChanged, other.m_locationChanged, options) != 0) {
        return false;
    }
    return true;
}
