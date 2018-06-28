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

#include "TestSharing.h"
#include "TestGlobal.h"
#include "stub/TestRandom.h"

#include <QBuffer>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "config-keepassx-tests.h"
#include "core/Metadata.h"
#include "crypto/Crypto.h"
#include "crypto/Random.h"
#include "crypto/ssh/OpenSSHKey.h"
#include "format/KeePass2Writer.h"
#include "keeshare/KeeShareSettings.h"
#include "keys/PasswordKey.h"

#include <format/KeePass2Reader.h>

QTEST_GUILESS_MAIN(TestSharing)

Q_DECLARE_METATYPE(KeeShareSettings::Type)
Q_DECLARE_METATYPE(KeeShareSettings::Key)
Q_DECLARE_METATYPE(KeeShareSettings::Certificate)
Q_DECLARE_METATYPE(QList<KeeShareSettings::Certificate>)
Q_DECLARE_METATYPE(Uuid)

void TestSharing::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestSharing::cleanupTestCase()
{
    TestRandom::teardown();
}

void TestSharing::testIdempotentDatabaseWriting()
{
    QScopedPointer<Database> db(new Database());

    Group* sharingGroup = new Group();
    sharingGroup->setName("SharingGroup");
    sharingGroup->setUuid(Uuid::random());
    sharingGroup->setParent(db->rootGroup());

    Entry* entry1 = new Entry();
    entry1->setUuid(Uuid::random());
    entry1->beginUpdate();
    entry1->setTitle("Entry1");
    entry1->endUpdate();
    entry1->setGroup(sharingGroup);

    Entry* entry2 = new Entry();
    entry2->setUuid(Uuid::random());
    entry2->beginUpdate();
    entry2->setTitle("Entry2");
    entry2->endUpdate();
    entry2->setGroup(sharingGroup);

    // prevent from changes introduced by randomization
    TestRandom::setup(new RandomBackendNull());

    QByteArray bufferOriginal;
    {
        QBuffer device(&bufferOriginal);
        device.open(QIODevice::ReadWrite);
        KeePass2Writer writer;
        writer.writeDatabase(&device, db.data());
    }

    QByteArray bufferCopy;
    {
        QBuffer device(&bufferCopy);
        device.open(QIODevice::ReadWrite);
        KeePass2Writer writer;
        writer.writeDatabase(&device, db.data());
    }

    QCOMPARE(bufferCopy, bufferOriginal);
}

void TestSharing::testNullObjects()
{
    const QString empty;
    QXmlStreamReader reader(empty);

    const KeeShareSettings::Key nullKey;
    QVERIFY(nullKey.isNull());
    const KeeShareSettings::Key xmlKey = KeeShareSettings::Key::deserialize(reader);
    QVERIFY(xmlKey.isNull());

    const KeeShareSettings::Certificate certificate;
    QVERIFY(certificate.isNull());
    const KeeShareSettings::Certificate xmlCertificate = KeeShareSettings::Certificate::deserialize(reader);
    QVERIFY(xmlCertificate.isNull());

    const KeeShareSettings::Own own;
    QVERIFY(own.isNull());
    const KeeShareSettings::Own xmlOwn = KeeShareSettings::Own::deserialize(empty);
    QVERIFY(xmlOwn.isNull());

    const KeeShareSettings::Active active;
    QVERIFY(active.isNull());
    const KeeShareSettings::Active xmlActive = KeeShareSettings::Active::deserialize(empty);
    QVERIFY(xmlActive.isNull());

    const KeeShareSettings::Foreign foreign;
    QVERIFY(foreign.isNull());
    const KeeShareSettings::Foreign xmlForeign = KeeShareSettings::Foreign::deserialize(empty);
    QVERIFY(xmlForeign.isNull());

    const KeeShareSettings::Reference reference;
    QVERIFY(reference.isNull());
    const KeeShareSettings::Reference xmlReference = KeeShareSettings::Reference::deserialize(empty);
    QVERIFY(xmlReference.isNull());
}

void TestSharing::testCertificateSerialization()
{
    QFETCH(bool, trusted);
    const OpenSSHKey& key = stubkey();
    KeeShareSettings::Certificate original;
    original.key = OpenSSHKey::serializeToBinary(OpenSSHKey::Public, key);
    original.signer = "Some <!> &#_\"\" weird string";
    original.trusted = trusted;

    QString buffer;
    QXmlStreamWriter writer(&buffer);
    writer.writeStartDocument();
    writer.writeStartElement("Certificate");
    KeeShareSettings::Certificate::serialize(writer, original);
    writer.writeEndElement();
    writer.writeEndDocument();
    QXmlStreamReader reader(buffer);
    reader.readNextStartElement();
    QVERIFY(reader.name() == "Certificate");
    KeeShareSettings::Certificate restored = KeeShareSettings::Certificate::deserialize(reader);

    QCOMPARE(restored.key, original.key);
    QCOMPARE(restored.signer, original.signer);
    QCOMPARE(restored.trusted, original.trusted);

    QCOMPARE(restored.sshKey().publicParts(), key.publicParts());
}

void TestSharing::testCertificateSerialization_data()
{
    QTest::addColumn<bool>("trusted");
    QTest::newRow("Trusted") << true;
    QTest::newRow("Untrusted") << false;
}

void TestSharing::testKeySerialization()
{
    const OpenSSHKey& key = stubkey();
    KeeShareSettings::Key original;
    original.key = OpenSSHKey::serializeToBinary(OpenSSHKey::Private, key);

    QString buffer;
    QXmlStreamWriter writer(&buffer);
    writer.writeStartDocument();
    writer.writeStartElement("Key");
    KeeShareSettings::Key::serialize(writer, original);
    writer.writeEndElement();
    writer.writeEndDocument();
    QXmlStreamReader reader(buffer);
    reader.readNextStartElement();
    QVERIFY(reader.name() == "Key");
    KeeShareSettings::Key restored = KeeShareSettings::Key::deserialize(reader);

    QCOMPARE(restored.key, original.key);
    QCOMPARE(restored.sshKey().privateParts(), key.privateParts());
    QCOMPARE(restored.sshKey().type(), key.type());
}

void TestSharing::testReferenceSerialization()
{
    QFETCH(QString, password);
    QFETCH(QString, path);
    QFETCH(Uuid, uuid);
    QFETCH(int, type);
    KeeShareSettings::Reference original;
    original.password = password;
    original.path = path;
    original.uuid = uuid;
    original.type = static_cast<KeeShareSettings::Type>(type);

    const QString serialized = KeeShareSettings::Reference::serialize(original);
    const KeeShareSettings::Reference restored = KeeShareSettings::Reference::deserialize(serialized);

    QCOMPARE(restored.password, original.password);
    QCOMPARE(restored.path, original.path);
    QCOMPARE(restored.uuid, original.uuid);
    QCOMPARE(int(restored.type), int(original.type));
}

void TestSharing::testReferenceSerialization_data()
{
    QTest::addColumn<QString>("password");
    QTest::addColumn<QString>("path");
    QTest::addColumn<Uuid>("uuid");
    QTest::addColumn<int>("type");
    QTest::newRow("1") << "Password" << "/some/path" << Uuid::random() << int(KeeShareSettings::Inactive);
    QTest::newRow("2") << "" << "" << Uuid() << int(KeeShareSettings::SynchronizeWith);
    QTest::newRow("3") << "" << "/some/path" << Uuid() << int(KeeShareSettings::ExportTo);

}

void TestSharing::testSettingsSerialization()
{
    QFETCH(bool, importing);
    QFETCH(bool, exporting);
    QFETCH(KeeShareSettings::Certificate, ownCertificate);
    QFETCH(KeeShareSettings::Key, ownKey);
    QFETCH(QList<KeeShareSettings::Certificate>, foreignCertificates);

    KeeShareSettings::Own originalOwn;
    KeeShareSettings::Foreign originalForeign;
    KeeShareSettings::Active originalActive;
    originalActive.in = importing;
    originalActive.out = exporting;
    originalOwn.certificate = ownCertificate;
    originalOwn.key = ownKey;
    originalForeign.certificates = foreignCertificates;

    const QString serializedActive = KeeShareSettings::Active::serialize(originalActive);
    KeeShareSettings::Active restoredActive = KeeShareSettings::Active::deserialize(serializedActive);

    const QString serializedOwn = KeeShareSettings::Own::serialize(originalOwn);
    KeeShareSettings::Own restoredOwn = KeeShareSettings::Own::deserialize(serializedOwn);

    const QString serializedForeign = KeeShareSettings::Foreign::serialize(originalForeign);
    KeeShareSettings::Foreign restoredForeign = KeeShareSettings::Foreign::deserialize(serializedForeign);

    QCOMPARE(restoredActive.in, importing);
    QCOMPARE(restoredActive.out, exporting);
    QCOMPARE(restoredOwn.certificate.key, ownCertificate.key);
    QCOMPARE(restoredOwn.certificate.trusted, ownCertificate.trusted);
    QCOMPARE(restoredOwn.key.key, ownKey.key);
    QCOMPARE(restoredForeign.certificates.count(), foreignCertificates.count());
    for (int i = 0; i < foreignCertificates.count(); ++i) {
        QCOMPARE(restoredForeign.certificates[i].key, foreignCertificates[i].key);
    }
}

void TestSharing::testSettingsSerialization_data()
{
    const OpenSSHKey& sshKey0 = stubkey(0);
    KeeShareSettings::Certificate certificate0;
    certificate0.key = OpenSSHKey::serializeToBinary(OpenSSHKey::Public, sshKey0);
    certificate0.signer = "Some <!> &#_\"\" weird string";
    certificate0.trusted = true;

    KeeShareSettings::Key key0;
    key0.key = OpenSSHKey::serializeToBinary(OpenSSHKey::Private, sshKey0);

    const OpenSSHKey& sshKey1 = stubkey(1);
    KeeShareSettings::Certificate certificate1;
    certificate1.key = OpenSSHKey::serializeToBinary(OpenSSHKey::Public, sshKey1);
    certificate1.signer = "Another ";
    certificate1.trusted = true;

    QTest::addColumn<bool>("importing");
    QTest::addColumn<bool>("exporting");
    QTest::addColumn<KeeShareSettings::Certificate>("ownCertificate");
    QTest::addColumn<KeeShareSettings::Key>("ownKey");
    QTest::addColumn<QList<KeeShareSettings::Certificate>>("foreignCertificates");
    QTest::newRow("1") << false << false << KeeShareSettings::Certificate() << KeeShareSettings::Key() << QList<KeeShareSettings::Certificate>();
    QTest::newRow("2") << true << false << KeeShareSettings::Certificate() << KeeShareSettings::Key() << QList<KeeShareSettings::Certificate>();
    QTest::newRow("3") << true << true << KeeShareSettings::Certificate() << KeeShareSettings::Key() << QList<KeeShareSettings::Certificate>({ certificate0, certificate1 });
    QTest::newRow("4") << false << true << certificate0 << key0 << QList<KeeShareSettings::Certificate>();
    QTest::newRow("5") << false << false << certificate0 << key0 << QList<KeeShareSettings::Certificate>({ certificate1 });
}

const OpenSSHKey& TestSharing::stubkey(int index)
{
    static QMap<int, OpenSSHKey*> keys;
    if (!keys.contains(index)) {
        OpenSSHKey* key = new OpenSSHKey(OpenSSHKey::generate(false));
        key->setParent(this);
        keys[index] = key;
    }
    return *keys[index];
}
