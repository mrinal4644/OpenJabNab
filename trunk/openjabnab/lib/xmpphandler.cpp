#include <QDateTime>
#include <QRegExp>
#include "bunny.h"
#include "bunnymanager.h"
#include "log.h"
#include "openjabnab.h"
#include "settings.h"
#include "xmpphandler.h"

unsigned short XmppHandler::msgNb = 0;

XmppHandler::XmppHandler(QTcpSocket * s):pluginManager(PluginManager::Instance())
{
	incomingXmppSocket = s;
	bunny = 0;
	
	// Bunny -> OpenJabNab socket
	connect(incomingXmppSocket, SIGNAL(disconnected()), this, SLOT(Disconnect()));
	incomingXmppSocket->setParent(this);

	// OpenJabNab -> Violet socket
	outgoingXmppSocket = new QTcpSocket(this);
	outgoingXmppSocket->connectToHost(GlobalSettings::GetString("DefaultVioletServers/XmppServer"), 5222);
	connect(outgoingXmppSocket, SIGNAL(connected()), this, SLOT(VioletConnected()));
	connect(outgoingXmppSocket, SIGNAL(readyRead()), this, SLOT(HandleVioletXmppMessage()));
	connect(outgoingXmppSocket, SIGNAL(disconnected()), this, SLOT(Disconnect()));
}

void XmppHandler::Disconnect()
{
	incomingXmppSocket->abort();
	outgoingXmppSocket->abort();
	if(bunny)
	{
		bunny->RemoveXmppHandler(this);
		bunny = 0;
	}
	deleteLater();
}

void XmppHandler::VioletConnected()
{
	if(incomingXmppSocket->bytesAvailable())
		HandleBunnyXmppMessage();
	connect(incomingXmppSocket, SIGNAL(readyRead()), this, SLOT(HandleBunnyXmppMessage()));
}

void XmppHandler::HandleBunnyXmppMessage()
{
	QByteArray data = incomingXmppSocket->readAll();
	
	// Replace OpenJabNab's domain
	data.replace(GlobalSettings::GetString("OpenJabNabServers/XmppServer").toAscii(),GlobalSettings::GetString("DefaultVioletServers/XmppDomain").toAscii());

	// If we don't already know which bunny is connected, try to find a <response></response> message
	if (!bunny)
	{
		QRegExp rx("<response[^>]*>(.*)</response>");
		if (rx.indexIn(data) != -1)
		{
			// Response message contains username, catch it to create the Bunny
			QByteArray authString = QByteArray::fromBase64(rx.cap(1).toAscii());
			rx.setPattern("username=[\'\"]([^\'\"]*)[\'\"]");
			if (rx.indexIn(authString) != -1)
			{
				QByteArray bunnyID = rx.cap(1).toAscii();
				bunny = BunnyManager::GetBunny(bunnyID);
				bunny->SetXmppHandler(this);
				bunny->SetGlobalSetting("Last JabberConnection", QDateTime::currentDateTime());
			}
			else
				Log::Warning(QString("Unable to parse response message : %1").arg(QString(authString)));
		}
	}

	// No bunny yet, forward
	if(!bunny)
	{
		// Send info to all 'system' plugins only
		pluginManager.XmppBunnyMessage(bunny, data);
		outgoingXmppSocket->write(data);
		return;
	}

	// Send info to all 'system' plugins and bunny's plugins
	bunny->XmppBunnyMessage(data);
	
	// Check if the data contains <message></message>
	QRegExp rx("<message[^>]*>(.*)</message>");
	if (rx.indexIn(data) == -1)
	{
		// Just some signaling informations, forward directly
		outgoingXmppSocket->write(data);
	}
	else
	{
		QString message = rx.cap(1);
		// Parse message
		bool handled = false;
		if (message.startsWith("<button"))
		{
			// Single Click : <button xmlns="violet:nabaztag:button"><clic>1</clic></button>
			// Double Click : <button xmlns="violet:nabaztag:button"><clic>2</clic></button>
			QRegExp rx("<clic>([0-9]+)</clic>");
			if (rx.indexIn(message) != -1)
			{
				int value = rx.cap(1).toInt();
				if (value == 1)
					handled = bunny->OnClick(PluginInterface::SingleClick);
				else if (value == 2)
					handled = bunny->OnClick(PluginInterface::DoubleClick);
				else
					Log::Warning(QString("Unable to parse button/click message : %1").arg(QString(data)));
			}
			else
				Log::Warning(QString("Unable to parse button message : %1").arg(QString(data)));
		}
		else if (message.startsWith("<ears"))
		{
			// <ears xmlns="violet:nabaztag:ears"><left>0</left><right>0</right></ears>
			QRegExp rx("<left>([0-9]+)</left><right>([0-9]+)</right>");
			if (rx.indexIn(message) != -1)
				handled = bunny->OnEarsMove(rx.cap(1).toInt(), rx.cap(2).toInt());
			else
				Log::Warning(QString("Unable to parse ears message : %1").arg(QString(data)));
		}
		else
			Log::Warning(QString("Unknown message from bunny : %1").arg(QString(data)));

		// If the message wasn't handled by a plugin, forward it to Violet
		if (!handled)
			outgoingXmppSocket->write(data);
	}
}

void XmppHandler::HandleVioletXmppMessage()
{
	QByteArray data = outgoingXmppSocket->readAll();

	// Replace Violet's domain
	data.replace(GlobalSettings::GetString("DefaultVioletServers/XmppDomain").toAscii(),GlobalSettings::GetString("OpenJabNabServers/XmppServer").toAscii());

	QList<QByteArray> list = XmlParse(data);
	foreach(QByteArray msg, list)
	{
		if(msg.startsWith("</stream:stream>"))
		{
			// Disconnect as soon as possible
			QTimer::singleShot(0, this, SLOT(Disconnect()));
			return;
		}

		if(!bunny)
		{
			// Send info to all 'system' plugins only and forward
			pluginManager.XmppVioletMessage(bunny, msg);
			WriteToBunny(msg);
			continue;
		}
		else
			bunny->XmppVioletMessage(msg); 	// Send info to all 'system' plugins and bunny's plugins


		// Check if the data contains <packet></packet>
		QRegExp rx("<packet[^>]*format='([^']*)'[^>]*>(.*)</packet>");
		if (rx.indexIn(msg) == -1)
		{
			// Just some signaling informations, forward directly
			WriteToBunny(msg);
		}
		else
		{
			bool drop = false;
			// Try to decode it
			try
			{
				if (rx.cap(1) != "1.0")
					throw QString("Unknown packet format : %1").arg(QString(msg));

				try
				{
					QList<Packet*> list = Packet::Parse(QByteArray::fromBase64(rx.cap(2).toAscii()));
					QList<Packet*>::iterator it = list.begin();
					while(it != list.end())
					{
						if (bunny->XmppVioletPacketMessage(**it))
						{
							delete *it; // Packed used and dropped
							it = list.erase(it);
							drop = true; // List changed
						}
						else
							it++;
					}
					if(drop) // If list changed
					{
						if(list.count()) // If there is still one packet to send
						{
							msg.replace(rx.cap(2), Packet::GetData(list));
							drop = false;
						}
						else
							drop = true; // Nothing to send, drop all
					}
					// Free Packets
					foreach(Packet * p, list)
						delete p;
				}
				catch (QString const& errorMsg)
				{
					Log::Warning(errorMsg);
				}
			}
			catch (QString const& errorMsg)
			{
				Log::Warning(errorMsg);
				// Can't handle it so forward it to the bunny
				WriteToBunny(msg);
			}
			if (!drop)
				WriteToBunny(msg);
		}
	}
}

void XmppHandler::WriteToBunny(QByteArray const& data)
{
	incomingXmppSocket->write(data);
	incomingXmppSocket->flush();
}

void XmppHandler::WriteDataToBunny(QByteArray const& b)
{
	if(bunny)
	{
		QByteArray msg;
		msg.append("<message from='net.openjabnab.platform@" + GlobalSettings::GetString("OpenJabNabServers/XmppServer").toAscii() + "/services' ");
		msg.append("to='" + bunny->GetID() + "@" + GlobalSettings::GetString("OpenJabNabServers/XmppServer").toAscii() + "/idle' ");
		msg.append("id='OJaNa-" + QByteArray::number(msgNb) + "'>");
		msg.append("<packet xmlns='violet:packet' format='1.0' ttl='604800'>");
		msg.append(b.toBase64());
		msg.append("</packet></message>");
		WriteToBunny(msg);
		msgNb++;
	}
}

QList<QByteArray> XmppHandler::XmlParse(QByteArray const& data)
{
	QList<QByteArray> list;
	msgQueue += data.trimmed();

	QRegExp rxStream("^(<\\?xml[^>]*\\?><stream:stream[^>]*>)");
	QRegExp rxOne("^(<[^>]*/>)");
	QRegExp rxEnd("^(</[^>]*>)");

	while(!msgQueue.isEmpty())
	{
		if (rxStream.indexIn(msgQueue) != -1)
		{
			list << rxStream.cap(1).toAscii();
			msgQueue.remove(0, rxStream.matchedLength());
		}
		else if (rxOne.indexIn(msgQueue) != -1)
		{
			list << rxOne.cap(1).toAscii();
			msgQueue.remove(0, rxOne.matchedLength());
		}
		else if (rxEnd.indexIn(msgQueue) != -1) // Special case for </stream:stream>
		{
			list << rxEnd.cap(1).toAscii();
			msgQueue.remove(0, rxEnd.matchedLength());
		}
		else
		{
			// Full xml message (<xxx>....</xxx>)
			// Find tag name
			QRegExp rxTag("^<([^ >]*)");
			if (rxTag.indexIn(msgQueue) == -1)
			{
				// Doesn't find the start tag... wait next message
				break;
			}
			QString tagName = rxTag.cap(1);
			// Search end tag
			rxTag.setPattern(QString("(.*</%1>)").arg(tagName));
			rxTag.setMinimal(true);
			if (rxTag.indexIn(msgQueue) == -1)
			{
				// Doesn't find the end tag... wait next message
				break;
			}
			list << rxTag.cap(1).toAscii();
			msgQueue.remove(0, rxTag.matchedLength());
		}
	}
	return list;
}
