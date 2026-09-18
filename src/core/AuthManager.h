#pragma once

#include <QString>

class AuthManager
{
public:
    // Generate self-signed SSL/TLS certificates for FreeRDP if they don't already exist
    static void generateCertificate();

    // Authenticate user credentials via PAM (or bypass if RDP_NO_AUTH / RDP_PASSWORD set)
    static bool authenticateUser(const QString& username, const QString& password);

    // Prepare temporary NTLM SAM database file for Windows mstsc NLA authentication
    static QString setupSamDatabase();

    // Remove temporary SAM database file
    static void cleanupSamDatabase(QString& samFilePath);
};
