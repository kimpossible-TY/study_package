from __future__ import annotations

import unittest

from browser_cleaner.developer_auth import CommandResult, CredentialEntry, DeveloperAuthService


class FakeRunner:
    def __init__(self, unavailable: bool = False) -> None:
        self.unavailable = unavailable
        self.commands: list[tuple[str, ...]] = []

    def run(self, command: tuple[str, ...]) -> CommandResult | None:
        self.commands.append(command)
        if self.unavailable:
            return None
        if command == ("gh", "auth", "status"):
            return CommandResult(0, "github.com\n  ✓ Logged in to github.com account octo-one (keyring)\n  ✓ Logged in to github.com account octo-two (keyring)\n", "")
        if command == ("git", "credential-manager", "github", "list"):
            return CommandResult(0, "username: octo-one\nusername: octo-two\n", "")
        if command[-2:] == ("tunnel", "status"):
            return CommandResult(0, "not registered\n", "")
        if "logout" in command or command[-2:] == ("tunnel", "unregister"):
            return CommandResult(0, "", "")
        return CommandResult(1, "", "")


class FakeCredentialStore:
    def __init__(self) -> None:
        self.entries = [CredentialEntry("git:https://github.com", "octo-one", 1)]
        self.deleted: list[str] = []

    def list_supported_entries(self):
        return list(self.entries)

    def delete(self, entry):
        self.deleted.append(entry.target_name)
        self.entries.remove(entry)
        return True


class FakeEnvironmentStore:
    def __init__(self) -> None:
        self.names = {"GH_TOKEN", "AZURE_TEST_TOKEN"}
        self.deleted: list[str] = []

    def list_token_names(self):
        return set(self.names)

    def audit_names(self):
        return sorted(self.names | {"GITHUB_TOKEN"})

    def delete(self, name):
        self.deleted.append(name)
        self.names.discard(name)
        return True


class DeveloperAuthTests(unittest.TestCase):
    def setUp(self) -> None:
        self.runner = FakeRunner()
        self.credentials = FakeCredentialStore()
        self.environment = FakeEnvironmentStore()
        self.service = DeveloperAuthService(
            runner=self.runner,
            credential_store=self.credentials,
            environment_store=self.environment,
            code_commands=("code",),
        )

    def test_audit_and_dry_run_handle_multiple_github_accounts_without_secrets(self) -> None:
        entries = self.service.audit()
        github_entries = [entry for entry in entries if entry.provider == "GitHub CLI"]
        self.assertEqual([entry.account_name for entry in github_entries], ["octo-one", "octo-two"])
        # Variable *names* are intentionally audited, but the fixture secret
        # value is neither passed to nor displayed by the service.
        self.assertNotIn("fixture-secret-value", "\n".join(str(entry) for entry in entries))

        results = self.service.remove_supported(False, lambda _subject, _phrase: True)

        self.assertTrue(results)
        self.assertTrue(all(result.status in {"dry-run", "unsupported"} for result in results))
        self.assertEqual(self.credentials.deleted, [])
        self.assertEqual(self.environment.deleted, [])

    def test_apply_uses_official_commands_and_per_item_confirmation(self) -> None:
        confirmations: list[tuple[str, str]] = []

        def confirm(subject: str, phrase: str) -> bool:
            confirmations.append((subject, phrase))
            return True

        results = self.service.remove_supported(True, confirm)

        self.assertTrue(all(result.status == "removed" for result in results))
        self.assertIn(("gh", "auth", "logout", "--hostname", "github.com", "--user", "octo-one"), self.runner.commands)
        self.assertIn(("gh", "auth", "logout", "--hostname", "github.com", "--user", "octo-two"), self.runner.commands)
        self.assertIn(("git", "credential-manager", "github", "logout", "octo-one"), self.runner.commands)
        self.assertEqual(self.credentials.deleted, ["git:https://github.com"])
        self.assertEqual(set(self.environment.deleted), {"GH_TOKEN", "AZURE_TEST_TOKEN"})
        self.assertGreaterEqual(len(confirmations), 6)

    def test_missing_commands_are_unsupported_not_fatal(self) -> None:
        service = DeveloperAuthService(
            runner=FakeRunner(unavailable=True),
            credential_store=FakeCredentialStore(),
            environment_store=FakeEnvironmentStore(),
            code_commands=("code",),
        )

        entries = service.audit()

        self.assertEqual(next(entry for entry in entries if entry.provider == "GitHub CLI").login_status, "unsupported")
        self.assertEqual(next(entry for entry in entries if entry.provider == "Git Credential Manager").login_status, "unsupported")

    def test_partial_official_logout_failure_does_not_hide_other_results(self) -> None:
        class PartialRunner(FakeRunner):
            def run(self, command):
                if command == ("gh", "auth", "logout", "--hostname", "github.com", "--user", "octo-two"):
                    self.commands.append(command)
                    return CommandResult(1, "", "")
                return super().run(command)

        runner = PartialRunner()
        credentials = FakeCredentialStore()
        environment = FakeEnvironmentStore()
        service = DeveloperAuthService(
            runner=runner,
            credential_store=credentials,
            environment_store=environment,
            code_commands=("code",),
        )

        results = service.remove_supported(True, lambda _subject, _phrase: True)

        failed = next(result for result in results if result.provider == "GitHub CLI" and result.subject == "octo-two")
        self.assertEqual(failed.status, "error")
        self.assertTrue(any(result.status == "removed" for result in results))
        self.assertEqual(credentials.deleted, ["git:https://github.com"])
        self.assertEqual(set(environment.deleted), {"GH_TOKEN", "AZURE_TEST_TOKEN"})


if __name__ == "__main__":
    unittest.main()
