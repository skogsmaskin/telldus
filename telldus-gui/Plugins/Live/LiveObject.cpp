#include "LiveObject.h"
#include "LiveMessageToken.h"
#include "config.h"
#include <QtNetwork>
#include <QtCore>
#include <QDesktopServices>
#include <QtCrypto>
#include <QMessageBox>

#include <QDebug>

class LiveObject::PrivateData {
public:
	class Server;

	QSslSocket *socket;
	QTimer timer;
	bool registered;
	QUrl registerUrl;
	QString uuid, hashMethod;
	QCA::Initializer qcaInit;
	QNetworkAccessManager *manager;
	QList<Server> serverList;
	QDateTime serverRefreshTime;
};

class LiveObject::PrivateData::Server {
public:
	QString address;
	int port;
};


LiveObject::LiveObject( QScriptEngine *engine, QObject * parent )
		: QObject(parent)
{
	d = new PrivateData;
	d->hashMethod = "sha1";
	foreach(QString hash, QStringList() << "sha512" << "sha256") {
		if (QCA::isSupported(hash.toLocal8Bit())) {
			d->hashMethod = hash;
			break;
		}
	}
	d->registered = false;
	d->socket = new QSslSocket(this);
	d->socket->setProtocol( QSsl::TlsV1 );
	connect(d->socket, SIGNAL(encrypted()), this, SLOT(p_connected()));
	connect(d->socket, SIGNAL(disconnected()), this, SLOT(p_disconnected()));
	connect(d->socket, SIGNAL(readyRead()), this, SLOT(readyRead()));
	connect(d->socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(error(QAbstractSocket::SocketError)));
	connect(d->socket, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this, SLOT(stateChanged(QAbstractSocket::SocketState)));
	connect(d->socket, SIGNAL(sslErrors(const QList<QSslError> &)), this, SLOT(sslErrors(const QList<QSslError> &)));

	d->timer.setInterval(60000); //Once a minute
	connect(&d->timer, SIGNAL(timeout()), this, SLOT(pingServer()));

	d->manager = new QNetworkAccessManager(this);
	connect(d->manager, SIGNAL(finished(QNetworkReply*)), this, SLOT(serverAssignReply(QNetworkReply*)));
}

LiveObject::~LiveObject() {
	delete d;
}

void LiveObject::activate() {
	if (d->uuid == "" || d->socket->state() != QAbstractSocket::ConnectedState || !d->registerUrl.isValid()) {
		return;
	}
	if (!QDesktopServices::openUrl(d->registerUrl)) {
		QMessageBox msgBox;
		msgBox.setIcon(QMessageBox::Information);
		msgBox.setTextFormat(Qt::RichText);
		msgBox.setWindowTitle(tr("Default browser could not be opened"));
		msgBox.setText(tr("We could not open your default internet browser"));
		msgBox.setInformativeText(tr("Please copy the following url and enter it in your browser:<br><a href='%1'>%1</a>").arg(d->registerUrl.toString()));
		msgBox.setStandardButtons(QMessageBox::Ok);
		msgBox.exec();
	}
}

void LiveObject::connectToServer() {
	if (d->serverList.isEmpty()) {
		this->refreshServerList();
	} else if (d->serverRefreshTime.secsTo(QDateTime::currentDateTime()) > 300) { //Valid 5 min
		this->refreshServerList();
	} else {
		d->socket->abort();
		PrivateData::Server server = d->serverList.takeFirst();
		qDebug() << "Connecting to" << server.address;
		d->socket->connectToHostEncrypted(server.address, server.port);
	}
}

void LiveObject::disconnect() {
	d->socket->disconnectFromHost();
}

void LiveObject::readyRead() {
	QByteArray ba = d->socket->readAll();
	qDebug() << ba;
	QScopedPointer<LiveMessage> envelope(LiveMessage::fromByteArray(ba));
	QString signature = envelope->name();
	QScopedPointer<LiveMessage> msg(LiveMessage::fromByteArray(envelope->arg(0).stringVal.toLocal8Bit()));

	if (signatureForMessage(envelope->arg(0).stringVal.toLocal8Bit()) != signature) {
		qDebug() << "HASH mismatch!" << msg->name();
		return;
	}

	if (msg->name() == "") {
		return;
	} else if (msg->name() == "disconnect") {
		this->disconnect();
	} else if (msg->name() == "registered") {
		d->registered = true;
		emit registered(msg->argument(0));
	} else if (msg->name() == "notregistered") {
		LiveMessageToken token = msg->arg(0);
		if (token.valueType != LiveMessageToken::Dictionary) {
			return;
		}
		d->registerUrl = token.dictVal["url"].stringVal;
		d->uuid = token.dictVal["uuid"].stringVal;
		QSettings s;
		s.setValue("Live/UUID", d->uuid);
		emit notRegistered();
	} else {
		emit messageReceived(msg.data());
	}
}

void LiveObject::refreshServerList() {
	d->serverList.clear();
	QUrl url(TELLDUS_LIVE_URI);
	QPair<QString, QString> version("protocolVersion", "1");
	QList<QPair<QString, QString> > query;
	query.append(version);
	url.setQueryItems(query);
	d->manager->get(QNetworkRequest(url));
}

void LiveObject::pingServer() {
	if (d->socket->state() != QAbstractSocket::ConnectedState) {
		return;
	}
	LiveMessage msg("Ping");
	this->sendMessage(msg);
}


void LiveObject::sendMessage(const LiveMessage &message) {
	//Create a new signed message
	QByteArray body = message.toByteArray();
	LiveMessage msg(signatureForMessage(body));
	msg.append(body);

	d->socket->write(msg.toByteArray());
	d->socket->flush();
}

void LiveObject::sendMessage(LiveMessage *message) {
	if (!d->registered) {
		return;
	}
	this->sendMessage(*message);
}

void LiveObject::p_connected() {
	//Lets find out our uuid
	QSettings settings;
	d->uuid = settings.value("Live/UUID", "").toString();

	d->timer.start(); //For pings
	LiveMessage msg("Register");

	LiveMessageToken token;
	token.valueType = LiveMessageToken::Dictionary;
	token.dictVal["uuid"] = LiveMessageToken(d->uuid);
	token.dictVal["key"] = LiveMessageToken(TELLDUS_LIVE_PUBLIC_KEY);
	token.dictVal["hash"] = LiveMessageToken(d->hashMethod);
	msg.append(token);
	msg.append(generateVersionToken());

	this->sendMessage(msg);
	emit connected();
}

void LiveObject::p_disconnected() {
	d->timer.stop();
	d->registered = false;
}

void LiveObject::error( QAbstractSocket::SocketError socketError ) {
	qDebug() << "Error:" << socketError;
}

void LiveObject::stateChanged( QAbstractSocket::SocketState socketState ) {
	if (socketState == QAbstractSocket::UnconnectedState) {
		QTimer::singleShot(10000, this, SLOT(connectToServer()));
		qDebug() << "Reconnect in 10 seconds...";
	} else if (socketState == QAbstractSocket::ConnectingState) {
		qDebug() << "Connecting...";
	} else {
		//qDebug() << "State:" << socketState;
	}
}

void LiveObject::sslErrors( const QList<QSslError> & errors ) {
	bool everythingOK = true;
	foreach( QSslError error, errors ) {
		switch( error.error() ) {
			case QSslError::SelfSignedCertificate:
				continue;
			default:
				everythingOK = false;
				qDebug() << "SSLErrors" << error;
				break;
		}
	}
	if (everythingOK) {
		d->socket->ignoreSslErrors();
	}
}

void LiveObject::serverAssignReply( QNetworkReply *r ) {
	qDebug() << "Server assign reply";
	QXmlStreamReader xml(r);
	xml.readNextStartElement(); // enter <servers>

	bool found = false;
	while (xml.readNextStartElement()) {
		if (xml.name().toString().toUpper() == "SERVER") {
			QXmlStreamAttributes attrs = xml.attributes();
			PrivateData::Server server;
			server.address = attrs.value("address").toString();
			server.port = attrs.value("port").toString().toInt();
			d->serverList << server;
			found = true;
		}
		xml.skipCurrentElement();
	}

	if (found) {
		qDebug() << "Servers found, retry direct...";
		d->serverRefreshTime = QDateTime::currentDateTime();
		QTimer::singleShot(0, this, SLOT(connectToServer()));
	} else {
		QTimer::singleShot(10000, this, SLOT(connectToServer()));
		qDebug() << "No server found, retry in 10 seconds...";
	}
	r->deleteLater();
}

QByteArray LiveObject::signatureForMessage( const QByteArray &message ) {
	if (QCA::isSupported(d->hashMethod.toLocal8Bit())) {
		QCA::Hash signature(d->hashMethod);
		signature.update(message);
		signature.update(TELLDUS_LIVE_PRIVATE_KEY);
		return signature.final().toByteArray().toHex();
	}

	//Fallback to builtin function
	QCryptographicHash signature( QCryptographicHash::Sha1 );
	signature.addData(message);
	signature.addData(TELLDUS_LIVE_PRIVATE_KEY);
	return signature.result().toHex();
}

LiveMessageToken LiveObject::generateVersionToken() {
	LiveMessageToken token;
	token.valueType = LiveMessageToken::Dictionary;
	token.dictVal["protocol"] = LiveMessageToken("1");
	token.dictVal["version"] = LiveMessageToken(TELLDUS_CENTER_VERSION);
#if defined(Q_WS_WIN)
	token.dictVal["os"] = LiveMessageToken("windows");
	switch (QSysInfo::WindowsVersion) {
		case QSysInfo::WV_XP:
			token.dictVal["os-version"] = LiveMessageToken("xp");
			break;
		case QSysInfo::WV_2003:
			token.dictVal["os-version"] = LiveMessageToken("2003");
			break;
		case QSysInfo::WV_VISTA:
			token.dictVal["os-version"] = LiveMessageToken("vista");
			break;
		case QSysInfo::WV_WINDOWS7:
			token.dictVal["os-version"] = LiveMessageToken("win7");
			break;
		default:
			token.dictVal["os-version"] = LiveMessageToken("unknown");
	}
#elif defined(Q_OS_MAC)
	token.dictVal["os"] = LiveMessageToken("macosx");
	switch (QSysInfo::MacintoshVersion) {
		case QSysInfo::MV_LEOPARD:
			token.dictVal["os-version"] = LiveMessageToken("leopard");
			break;
		case QSysInfo::MV_SNOWLEOPARD:
			token.dictVal["os-version"] = LiveMessageToken("snowleopard");
			break;
		default:
			token.dictVal["os-version"] = LiveMessageToken("unknown");
	 }
#elif defined(Q_OS_LINUX)
	token.dictVal["os"] = LiveMessageToken("linux");
	token.dictVal["os-version"] = LiveMessageToken("unknown");
#else
	token.dictVal["os"] = LiveMessageToken("unknown");
	token.dictVal["os-version"] = LiveMessageToken("");
#endif
	return token;
}