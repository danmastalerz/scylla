======================================================================
Upgrade Guide - Scylla 1.x.y to 1.x.z for |OS|
======================================================================

This document is a step by step procedure for upgrading from Scylla 1.x.y to Scylla 1.x.z.


Applicable versions
===================
This guide covers upgrading Scylla from the following versions: 1.x.y to Scylla version 1.x.z on the following platform:

* |OS|

Upgrade Procedure
=================

.. include:: /upgrade/_common/warning.rst

A Scylla upgrade is a rolling procedure which does **not** require full cluster shutdown.
For each of the nodes in the cluster, you will:

* drain node and backup the data
* check your current release
* backup configuration file and deb packages
* stop Scylla
* download and install new Scylla packages
* start Scylla
* validate that the upgrade was successful

Apply the following procedure **serially** on each node. Do not move to the next node before validating the node is up and running with the new version.

**During** the rolling upgrade it is highly recommended:

* Not to use new 1.x.z features
* Not to run administration functions, like repairs, refresh, rebuild or add or remove nodes
* Not to apply schema changes

Upgrade steps
=============
Drain node and backup the data
------------------------------
Before any major procedure, like an upgrade, it is recommended to backup all the data to an external device. In Scylla, backup is done using the ``nodetool snapshot`` command. For **each** node in the cluster, run the following command:

.. code:: sh

   nodetool drain
   nodetool snapshot

Take note of the directory name that nodetool gives you, and copy all the directories having this name under ``/var/lib/scylla`` to a backup device.

When the upgrade is complete (all nodes), the snapshot should be removed by ``nodetool clearsnapshot -t <snapshot>``, or you risk running out of space.

Backup configuration file and deb packages
------------------------------------------

.. code:: sh

   sudo cp -a /etc/scylla/scylla.yaml /etc/scylla/scylla.yaml.backup-1.x.z

If you install scylla by apt, you can find the deb packages in ``/var/cache/apt/``, backup them to ``scylla_1.x.y_backup`` directory which will be used in rollback.

Gracefully stop the node
------------------------

.. code:: sh

   sudo service scylla-server stop

Download and install the new release
------------------------------------
Before upgrading, check what version you are running now using ``dpkg -s scylla-server``. You should use the same version in case you want to |ROLLBACK|_ the upgrade. If you are not running a 1.x.y version, stop right here! This guide only covers 1.x.y to 1.x.z upgrades.

To upgrade:

1. Update the |APT|_ to **1.x**
2. Install

.. code:: sh

   sudo apt-get update
   sudo apt-get dist-upgrade scylla

Answer ‘y’ to the first two questions.

Start the node
--------------

.. code:: sh

   sudo service scylla-server start

Validate
--------
1. Check cluster status with ``nodetool status`` and make sure **all** nodes, including the one you just upgraded, are in UN status.
2. Use ``curl -X GET "http://localhost:10000/storage_service/scylla_release_version"`` to check scylla version.
3. Check scylla-server log (check ``/var/log/upstart/scylla-server.log`` for Ubuntu 14.04, execute ``journalctl _COMM=scylla`` for Ubuntu 16.04) and ``/var/log/syslog`` to validate there are no errors.
4. Check again after 2 minutes, to validate no new issues are introduced.

Once you are sure the node upgrade is successful, move to the next node in the cluster.

Rollback Procedure
==================

.. include:: /upgrade/_common/warning_rollback.rst

The following procedure describes a rollback from Scylla release 1.x.z to 1.x.y. Apply this procedure if an upgrade from 1.x.y to 1.x.z failed before completing on all nodes. Use this procedure only for nodes you upgraded to 1.x.z

Scylla rollback is a rolling procedure which does **not** require full cluster shutdown.
For each of the nodes rollback to 1.x.y, you will:

* drain the node and stop Scylla
* retrieve the old Scylla packages
* restore the configuration file
* restart Scylla
* validate the rollback success

Apply the following procedure **serially** on each node. Do not move to the next node before validating the node is up and running with the new version.

Rollback steps
==============
Gracefully shutdown Scylla
--------------------------

.. code:: sh

   nodetool drain
   sudo service scylla-server stop

Install the old release from backuped deb packages
--------------------------------------------------
1. install

.. code:: sh

   sudo apt-get remove scylla\* -y
   sudo apt-get install scylla_1.x.y_backup/scylla*.deb

Answer ‘y’ to the first two questions.

Restore the configuration file
------------------------------

.. code:: sh

   sudo rm -rf /etc/scylla/scylla.yaml
   sudo cp -a /etc/scylla/scylla.yaml.backup-1.x.z /etc/scylla/scylla.yaml

Start the node
--------------

.. code:: sh

   sudo service scylla-server start

Validate
--------
Check upgrade instruction above for validation. Once you are sure the node rollback is successful, move to the next node in the cluster.
