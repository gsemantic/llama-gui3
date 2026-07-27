#pragma once

#include <string>
#include <functional>
#include <memory>

namespace llama_gui {
namespace ui {

// =========================================================================
// Базовый класс команды
// =========================================================================

/**
 * @brief Базовый класс для всех команд в системе
 * Команды инкапсулируют действия для выполнения, отмены и проверки доступности
 */
class Command {
public:
    virtual ~Command() = default;

    // Выполнить команду
    virtual void execute() = 0;

    // Отменить команду (если поддерживается)
    virtual void undo() {
        // По умолчанию отмена не поддерживается
    }

    // Проверить, можно ли выполнить команду
    virtual bool canExecute() const {
        return true;
    }

    // Получить имя команды
    virtual std::string getName() const = 0;

    // Получить описание команды
    virtual std::string getDescription() const {
        return "";
    }

    // Получить горячую клавишу
    virtual std::string getShortcut() const {
        return "";
    }

    // Проверить, требует ли команда подтверждения
    virtual bool requiresConfirmation() const {
        return false;
    }

    // Получить текст подтверждения
    virtual std::string getConfirmationText() const {
        return "";
    }
};

// =========================================================================
// Функциональная команда (для простых лямбда-функций)
// =========================================================================

/**
 * @brief Команда на основе функциональных объектов
 * Позволяет создавать команды из лямбда-выражений и std::function
 */
class FunctionalCommand : public Command {
private:
    std::string name_;
    std::string description_;
    std::string shortcut_;
    std::function<void()> execute_func_;
    std::function<bool()> can_execute_func_;

public:
    FunctionalCommand(
        const std::string& name,
        std::function<void()> execute_func,
        const std::string& description = "",
        const std::string& shortcut = "",
        std::function<bool()> can_execute_func = nullptr
    ) : name_(name), execute_func_(std::move(execute_func)),
        description_(description), shortcut_(shortcut),
        can_execute_func_(std::move(can_execute_func)) {}

    void execute() override {
        if (execute_func_) {
            execute_func_();
        }
    }

    bool canExecute() const override {
        if (can_execute_func_) {
            return can_execute_func_();
        }
        return true;
    }

    std::string getName() const override {
        return name_;
    }

    std::string getDescription() const override {
        return description_;
    }

    std::string getShortcut() const override {
        return shortcut_;
    }
};

} // namespace ui
} // namespace llama_gui
